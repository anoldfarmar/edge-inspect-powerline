#include "edgeinspect/task_manager.h"
#include "edgeinspect/task_queue.h"
#include "edgeinspect/task_repository.h"
#include "edgeinspect/file_storage.h"
#include <sqlite3.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

using namespace edgeinspect;
using namespace std::chrono_literals;
namespace {
void require(bool yes, const std::string& message) { if (!yes) throw std::runtime_error(message); }
struct Temp {
    std::filesystem::path path;
    Temp() {
        auto pattern = (std::filesystem::temp_directory_path() / "edgeinspect-stage2-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void put(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary); f << text;
    require(bool(f), "write test fixture");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
template<class F> void rejects(const std::string& code, F action) {
    try { action(); } catch (const TaskError& e) { require(e.code() == code, "Expected " + code + ", got " + e.code()); return; }
    throw std::runtime_error("Expected error " + code);
}
void sql(const std::filesystem::path& path, const char* text) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open injection db");
    const int rc = sqlite3_exec(db, text, nullptr, nullptr, nullptr);
    const std::string error = sqlite3_errmsg(db);
    sqlite3_close(db);
    require(rc == SQLITE_OK, "execute injection SQL: " + error);
}
TaskRecord terminal(TaskManager& m, const std::string& id) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < until) {
        auto t = m.get(id);
        if (t.status == TaskStatus::Succeeded || t.status == TaskStatus::Failed) return t;
        if (m.fault()) throw std::runtime_error("Unexpected fault: " + *m.fault());
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Timed out waiting for task");
}
TaskRecord fresh(std::string id) {
    TaskRecord t; t.id = std::move(id); t.asset = "glass-insulator";
    t.input_path = "tasks/example/input.bin"; t.created_at = utc_now(); return t;
}
void repository_checks() {
    Temp tmp; const auto db = tmp.path / "tasks.sqlite3";
    {
        TaskRepository r(db);
        auto task = fresh("id'quoted");
        r.insert_queued(task);
        auto loaded = r.get(task.id);
        require(loaded.id == task.id && loaded.status == TaskStatus::Queued && !loaded.started_at && !loaded.result_path, "insert/get roundtrip");
        rejects("DATABASE_ERROR", [&] { r.insert_queued(task); });
        rejects("NOT_FOUND", [&] { r.get("missing"); });
        require(!r.mark_succeeded(task.id,"result.json"), "cannot skip running");
        require(r.mark_running(task.id) && !r.mark_running(task.id), "conditional running");
        require(r.get(task.id).started_at.has_value(), "start timestamp");
        require(r.mark_succeeded(task.id,"result.json"), "mark success");
        require(!r.mark_failed(task.id,"BAD","late"), "terminal state cannot change");
        auto b = fresh("failed"); r.insert_queued(b); require(r.mark_running(b.id), "start failure task");
        require(r.mark_failed(b.id,"INVALID_INPUT","broken ' image"), "mark failure");
        require(r.get(b.id).error_message == "broken ' image", "error binding");
        require(!r.has_incomplete() && r.list().size() == 2, "list and incomplete");
        rejects("INVALID_ARGUMENT", [&] { r.list(0); });
        auto invalid = fresh("invalid"); invalid.status = TaskStatus::Running;
        rejects("INVALID_ARGUMENT", [&] { r.insert_queued(invalid); });
    }
    TaskRepository reopened(db);
    require(reopened.get("id'quoted").finished_at.has_value(), "persistent history");
    require(reopened.get("failed").status == TaskStatus::Failed, "persistent failure");
    require(task_status_from_string("QUEUED") == TaskStatus::Queued, "status parse");
    bool invalid = false;
    try { task_status_from_string("Runing"); } catch (const std::invalid_argument&) { invalid = true; }
    require(invalid, "unknown status rejected");
}
void queue_checks() {
    TaskQueue q(2); bool callback = false;
    require(q.try_push({"A","a","p"}) && q.try_push({"B","a","p"}), "queue fill");
    require(!q.try_push({"C","a","p"}, [&] { callback = true; }) && !callback, "full queue never calls admission");
    require(q.wait_pop()->id == "A", "FIFO");
    require(q.try_push({"C","a","p"}), "ring wrap");
    require(q.wait_pop()->id == "B" && q.wait_pop()->id == "C", "wrapped FIFO");
    bool threw = false;
    try { q.try_push({"D","a","p"}, [] { throw std::runtime_error("injected insert failure"); }); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw && q.size() == 0, "failed admission does not publish");
    auto waiting = std::async(std::launch::async, [&] { return q.wait_pop(); });
    q.close();
    require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get(), "close wakes empty consumer");
    require(!q.try_push({"E","a","p"}), "closed queue refuses");
    TaskQueue drain(1); drain.try_push({"F","a","p"}); drain.close();
    require(drain.wait_pop()->id == "F" && !drain.wait_pop(), "close drains accepted tasks");
}
void storage_checks() {
    Temp tmp; auto source = tmp.path / "source.jpg"; put(source,"original");
    FileStorage fs(tmp.path / "data");
    rejects("ALREADY_RUNNING", [&] { FileStorage duplicate(tmp.path / "data"); });
    const auto id = fs.copy_input(source,100);
    put(source,"changed");
    require(read(fs.resolve(fs.input_relative(id))) == "original", "immutable saved copy");
    rejects("INVALID_INPUT", [&] { fs.copy_input(source,1); });
    put(source,""); rejects("INVALID_INPUT", [&] { fs.copy_input(source,100); });
    rejects("INVALID_ARGUMENT", [&] { fs.resolve("../etc/passwd"); });
    fs.write_result(id,"{\"score\":0.5}");
    rejects("STORAGE_ERROR", [&] { fs.write_result(id,"overwrite"); });
    require(fs.read_result(fs.result_relative(id)) == "{\"score\":0.5}", "no overwrite");
    fs.remove_task(id);
    require(std::filesystem::is_empty(fs.root() / "tasks"), "cleanup");
}
void manager_checks() {
    Temp tmp; const auto source = tmp.path / "source.jpg"; put(source,"original");
    TaskManagerConfig config; config.data_root = tmp.path / "data"; config.queue_capacity = 2;
    std::promise<void> entered, release; auto gate = release.get_future().share();
    std::atomic<int> starts{0}, calls{0}, active{0}, maximum{0};
    ProcessorFactory factory = [&] {
        ++starts;
        return TaskProcessor([&](const std::filesystem::path& path, const std::string&) {
            const int current = ++active; maximum.store(std::max(maximum.load(), current));
            const int n = ++calls;
            if (n == 1) { entered.set_value(); gate.wait(); }
            const auto contents = read(path);
            --active;
            if (contents == "bad") throw TaskError("INVALID_INPUT","broken image");
            return std::string("{\"contents\":") + json_string(contents) + "}";
        });
    };
    {
        TaskManager m(config,factory);
        rejects("NOT_ACCEPTING", [&] { m.submit(source,"glass-insulator"); });
        m.start();
        const auto a = m.submit(source,"glass-insulator");
        require(entered.get_future().wait_for(2s) == std::future_status::ready, "worker starts");
        put(source,"bad"); const auto b = m.submit(source,"glass-insulator");
        put(source,"after"); const auto c = m.submit(source,"glass-insulator");
        rejects("QUEUE_FULL", [&] { m.submit(source,"glass-insulator"); });
        require(m.list().size() == 3, "full rejection leaves no row");
        require(std::distance(std::filesystem::directory_iterator(config.data_root / "tasks"), std::filesystem::directory_iterator{}) == 3, "full rejection removes input directory");
        require(m.get(a).status == TaskStatus::Running, "running state visible");
        rejects("INVALID_ASSET", [&] { m.submit(source,"unknown"); });
        rejects("NOT_READY", [&] { m.result(b); });
        release.set_value();
        require(terminal(m,a).status == TaskStatus::Succeeded, "A success");
        require(terminal(m,b).status == TaskStatus::Failed && m.get(b).error_code == "INVALID_INPUT", "B failure recorded");
        require(terminal(m,c).status == TaskStatus::Succeeded, "C continues after bad input");
        require(m.result(a) == "{\"contents\":\"original\"}", "input snapshot used");
        require(m.result(c) == "{\"contents\":\"after\"}", "result retrieval");
        m.stop_and_drain();
        require(!m.fault() && starts == 1 && calls == 3 && maximum == 1, "one initialized nonconcurrent processor");
        rejects("NOT_ACCEPTING", [&] { m.submit(source,"glass-insulator"); });
    }
    { TaskManager reopened(config,factory); reopened.start(); require(reopened.list().size() == 3, "history survives restart"); reopened.stop_and_drain(); }
    // Drain actual queued work on EOF/quit rather than dropping it.
    config.data_root = tmp.path / "drain";
    { TaskManager m(config, [] { return TaskProcessor([](const auto&,const auto&) { return "{}"; }); });
      m.start(); m.submit(source,"glass-insulator"); m.submit(source,"glass-insulator"); m.stop_and_drain();
      for (auto& row : m.list()) require(row.status == TaskStatus::Succeeded, "shutdown drains tasks"); }
    config.data_root = tmp.path / "concurrent";
    config.queue_capacity = 32;
    { TaskManager m(config, [] { return TaskProcessor([](const auto&,const auto&) { return "{}"; }); });
      m.start();
      std::vector<std::future<void>> producers;
      for (int i = 0; i < 4; ++i) producers.push_back(std::async(std::launch::async, [&] {
          for (int j = 0; j < 5; ++j) m.submit(source,"glass-insulator");
      }));
      for (auto& producer : producers) producer.get();
      m.stop_and_drain();
      const auto rows = m.list(100);
      require(rows.size() == 20 && !m.fault(), "concurrent submitters accepted exactly once");
      for (const auto& row : rows) require(row.status == TaskStatus::Succeeded, "concurrent tasks drain");
    }
}
void failure_checks() {
    Temp tmp; auto source = tmp.path / "source"; put(source,"x");
    TaskManagerConfig config; config.data_root = tmp.path / "startup";
    { TaskManager m(config, []() -> TaskProcessor { throw std::runtime_error("injected initialization failure"); });
      bool failed = false; try { m.start(); } catch (const std::runtime_error&) { failed = true; }
      require(failed && m.fault(), "startup failure handshake"); }
    config.data_root = tmp.path / "incomplete"; std::filesystem::create_directories(config.data_root);
    { TaskRepository r(config.data_root / "tasks.sqlite3"); r.insert_queued(fresh("unfinished")); }
    { TaskManager m(config, [] { return TaskProcessor([](const auto&,const auto&) { return "{}"; }); });
      rejects("INCOMPLETE_TASKS", [&] { m.start(); }); }
    config.data_root = tmp.path / "db-failure";
    { TaskManager m(config, [] { return TaskProcessor([](const auto&,const auto&) { return "{}"; }); });
      m.start();
      sql(config.data_root / "tasks.sqlite3", "CREATE TRIGGER refuse_insert BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'injected insert failure'); END;");
      rejects("DATABASE_ERROR", [&] { m.submit(source,"glass-insulator"); });
      require(m.list().empty() && m.fault(), "failed admission no row and faults service");
      require(std::filesystem::is_empty(config.data_root / "tasks"), "failed admission cleanup"); }
    config.data_root = tmp.path / "update-failure";
    { TaskManager m(config, [] { return TaskProcessor([](const auto&,const auto&) { return "{}"; }); });
      m.start();
      sql(config.data_root / "tasks.sqlite3", "CREATE TRIGGER refuse_success BEFORE UPDATE OF status ON tasks WHEN NEW.status='SUCCEEDED' BEGIN SELECT RAISE(ABORT,'injected update failure'); END;");
      const auto id = m.submit(source,"glass-insulator"); m.stop_and_drain();
      require(m.fault() && m.get(id).status == TaskStatus::Running, "final database failure cannot report success");
      require(std::filesystem::exists(config.data_root / "tasks" / id / "result.json"), "preserve result for later reconciliation"); }
    config.data_root = tmp.path / "save-failure";
    { TaskManager m(config, [] { return TaskProcessor([](const std::filesystem::path& path,const auto&) {
          std::filesystem::create_directory(path.parent_path() / "result.json"); return "{}";
      }); });
      m.start(); const auto id = m.submit(source,"glass-insulator"); m.stop_and_drain();
      require(!m.fault() && m.get(id).status == TaskStatus::Failed && m.get(id).error_code == "STORAGE_ERROR", "save failure is failed task"); }
}
}
int main() {
    try {
        repository_checks(); queue_checks(); storage_checks(); manager_checks(); failure_checks();
        std::cout << "Stage2 repository/queue/storage/manager checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
