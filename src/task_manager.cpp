#include "edgeinspect/task_manager.h"
#include "edgeinspect/file_storage.h"
#include "edgeinspect/task_queue.h"
#include "edgeinspect/task_repository.h"
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

namespace edgeinspect {
namespace {
void require_transition(bool changed) {
    if (!changed) throw TaskError("DATABASE_ERROR", "Task state transition affected no row");
}
ProcessorFactory real_processor(const Config& config) {
    return [config] {
        // Construction, all calls, and final destruction occur on the worker.
        auto engine = std::make_shared<InferenceEngine>(config);
        return [config, engine](const std::filesystem::path& path, const std::string& asset) {
            PreparedInput input;
            try { input = prepare_image(path, config); }
            catch (const std::exception& e) { throw TaskError("INVALID_INPUT", e.what()); }
            try {
                const auto result = engine->infer(input, asset, false);
                return result_json(result, input, engine->environment_json());
            } catch (const std::exception& e) { throw TaskError("INFERENCE_ERROR", e.what()); }
        };
    };
}
}
struct TaskManager::Impl {
    enum class State { Constructed, Starting, Ready, Draining, Stopped, Faulted };
    TaskManagerConfig config;
    FileStorage storage;              // Outlives repository; holds data_root lock.
    TaskRepository repository;
    TaskQueue queue;
    ProcessorFactory factory;
    mutable std::mutex state_mutex;
    std::mutex lifecycle_mutex;
    State state = State::Constructed;
    std::optional<std::string> error;
    std::thread worker;

    Impl(TaskManagerConfig c, ProcessorFactory f)
        : config(std::move(c)), storage(config.data_root),
          repository(storage.root() / "tasks.sqlite3"), queue(config.queue_capacity),
          factory(f ? std::move(f) : real_processor(config.inference)) {}

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(state_mutex);
        error = message; state = State::Faulted; queue.close();
        std::cerr << "Task service fault: " << message << '\n';
    }
    void run(std::promise<void>& ready) noexcept {
        bool announced = false;
        try {
            auto process = factory();
            if (!process) throw TaskError("STARTUP_ERROR", "Processor factory returned no processor");
            { std::lock_guard<std::mutex> lock(state_mutex); state = State::Ready; }
            ready.set_value(); announced = true;
            while (auto task = queue.wait_pop()) {
                { std::lock_guard<std::mutex> lock(state_mutex); if (state == State::Faulted) break; }
                require_transition(repository.mark_running(task->id));
                try {
                    auto json = process(storage.resolve(task->input_path), task->asset);
                    storage.write_result(task->id, json);
                } catch (const TaskError& e) {
                    require_transition(repository.mark_failed(task->id, e.code(), e.what()));
                    if (e.code() == "INVALID_INPUT" || e.code() == "STORAGE_ERROR") continue;
                    throw;
                } catch (const std::exception& e) {
                    require_transition(repository.mark_failed(task->id, "INFERENCE_ERROR", e.what()));
                    throw;
                }
                // A failure here leaves the written file intact and faults the service.
                require_transition(repository.mark_succeeded(task->id, storage.result_relative(task->id)));
            }
        } catch (...) {
            const auto failure = std::current_exception();
            try { std::rethrow_exception(failure); }
            catch (const std::exception& e) { fail(e.what()); }
            catch (...) { fail("Unknown worker exception"); }
            if (!announced) { try { ready.set_exception(failure); } catch (...) {} }
        }
    }
    void stop() {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (state != State::Faulted) state = State::Draining;
            queue.close();
        }
        if (worker.joinable()) worker.join();
        { std::lock_guard<std::mutex> lock(state_mutex); if (state != State::Faulted) state = State::Stopped; }
    }
};
TaskManager::TaskManager(TaskManagerConfig config, ProcessorFactory factory)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(factory))) {}
TaskManager::~TaskManager() { impl_->stop(); }
void TaskManager::start() {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->state != Impl::State::Constructed) throw TaskError("NOT_ACCEPTING", "TaskManager can only start once");
        if (impl_->repository.has_incomplete())
            throw TaskError("INCOMPLETE_TASKS", "Unfinished tasks exist; phase 2 does not recover them. Inspect this data_root or choose a fresh one.");
        impl_->state = Impl::State::Starting;
    }
    std::promise<void> ready;
    auto status = ready.get_future();
    try {
        impl_->worker = std::thread([this, ready = std::move(ready)]() mutable { impl_->run(ready); });
        status.get();
    } catch (...) {
        if (impl_->worker.joinable()) impl_->worker.join();
        throw;
    }
}
std::string TaskManager::submit(const std::filesystem::path& image, const std::string& asset) {
    try { (void)asset_class(asset); }
    catch (const std::invalid_argument& e) { throw TaskError("INVALID_ASSET", e.what()); }
    // Serialize admission and stop/fault state changes. No lock is held by infer().
    std::unique_lock<std::mutex> lock(impl_->state_mutex);
    if (impl_->state != Impl::State::Ready) throw TaskError("NOT_ACCEPTING", "Task service is not accepting submissions");
    auto id = impl_->storage.copy_input(image, impl_->config.inference.max_image_bytes);
    try {
        TaskRecord record;
        record.id = id; record.asset = asset;
        record.input_path = impl_->storage.input_relative(id); record.created_at = utc_now();
        QueuedTask queued{id, asset, record.input_path};
        // While the queue lock is held, commit the record then publish via noexcept moves.
        // No later operation before return can throw under normal operation.
        const bool accepted = impl_->queue.try_push(std::move(queued), [&] { impl_->repository.insert_queued(record); });
        if (!accepted) throw TaskError("QUEUE_FULL", "Waiting task queue is full");
    } catch (...) {
        const auto original = std::current_exception();
        try { impl_->storage.remove_task(id); }
        catch (const std::exception& e) {
            impl_->error = e.what(); impl_->state = Impl::State::Faulted; impl_->queue.close();
            throw;
        }
        try { std::rethrow_exception(original); }
        catch (const TaskError& e) {
            if (e.code() == "DATABASE_ERROR") {
                impl_->error = e.what(); impl_->state = Impl::State::Faulted; impl_->queue.close();
            }
            throw;
        }
    }
    return id;
}
TaskRecord TaskManager::get(const std::string& id) { return impl_->repository.get(id); }
std::vector<TaskRecord> TaskManager::list(std::size_t limit) { return impl_->repository.list(limit); }
std::string TaskManager::result(const std::string& id) {
    const auto task = get(id);
    if (task.status != TaskStatus::Succeeded || !task.result_path)
        throw TaskError("NOT_READY", "Task has no successful result");
    return impl_->storage.read_result(*task.result_path);
}
void TaskManager::stop_and_drain() { impl_->stop(); }
std::optional<std::string> TaskManager::fault() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex); return impl_->error;
}
} // namespace edgeinspect
