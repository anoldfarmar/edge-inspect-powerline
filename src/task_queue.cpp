#include "edgeinspect/task_queue.h"
#include <type_traits>

namespace edgeinspect {
TaskQueue::TaskQueue(std::size_t capacity) : slots_(capacity) {
    if (!capacity) throw TaskError("INVALID_ARGUMENT", "Queue capacity must be positive");
    static_assert(std::is_nothrow_move_constructible<QueuedTask>::value,
                  "Publication after database commit must not throw");
}
bool TaskQueue::try_push(QueuedTask task, const std::function<void()>& before_publish) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || count_ == slots_.size()) return false;
    // The worker cannot pop while this runs. If it throws, nothing is published.
    // Must be short; never call back into this queue from before_publish.
    if (before_publish) before_publish();
    slots_[tail_].emplace(std::move(task));
    tail_ = (tail_ + 1) % slots_.size();
    ++count_;
    lock.unlock();
    readable_.notify_one();
    return true;
}
std::optional<QueuedTask> TaskQueue::wait_pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    readable_.wait(lock, [&] { return closed_ || count_; });
    if (!count_) return std::nullopt;
    auto task = std::move(slots_[head_]);
    slots_[head_].reset();
    head_ = (head_ + 1) % slots_.size();
    --count_;
    return task;
}
void TaskQueue::close() {
    { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
    readable_.notify_all();
}
std::size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}
} // namespace edgeinspect
