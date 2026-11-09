#include "edgeinspect/task_repository.h"
#include <sqlite3.h>
#include <iostream>
#include <limits>
#include <mutex>

namespace edgeinspect {
namespace {
[[noreturn]] void database_error(sqlite3* db, const std::string& operation) {
    throw TaskError("DATABASE_ERROR", operation + ": " + (db ? sqlite3_errmsg(db) : "no connection"));
}
void check(int rc, sqlite3* db, const char* operation) {
    if (rc != SQLITE_OK) database_error(db, operation);
}
void text_argument(const std::string& x) {
    if (x.find('\0') != std::string::npos || x.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw TaskError("INVALID_ARGUMENT", "Invalid text argument");
}
struct DatabaseCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db && sqlite3_close(db) != SQLITE_OK) std::cerr << "sqlite3_close failed\n";
    }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* s) const noexcept { if (s) sqlite3_finalize(s); }
};
// Keep every prepared statement scoped; no SQLite pointer escapes Repository.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        sqlite3_stmt* raw = nullptr;
        const int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
        value_.reset(raw);
        check(rc, db, "Prepare SQL");
    }
    void bind(int index, const std::string& x) {
        text_argument(x);
        check(sqlite3_bind_text(value_.get(), index, x.data(), static_cast<int>(x.size()), SQLITE_TRANSIENT), db_, "Bind text");
    }
    void bind_limit(std::size_t limit) {
        check(sqlite3_bind_int64(value_.get(), 1, static_cast<sqlite3_int64>(limit)), db_, "Bind limit");
    }
    int step() {
        const int rc = sqlite3_step(value_.get());
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) database_error(db_, "Execute SQL");
        return rc;
    }
    void execute() { if (step() != SQLITE_DONE) database_error(db_, "Expected no result rows"); }
    std::optional<std::string> optional(int column) {
        if (sqlite3_column_type(value_.get(), column) == SQLITE_NULL) return std::nullopt;
        const auto* text = sqlite3_column_text(value_.get(), column);
        if (!text) database_error(db_, "Read text");
        return std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(value_.get(), column));
    }
    std::string required(int column) {
        auto x = optional(column);
        if (!x) throw TaskError("DATABASE_ERROR", "Unexpected NULL in required task field");
        return *x;
    }
    TaskRecord record() {
        TaskRecord t;
        t.id = required(0); t.asset = required(1);
        try { t.status = task_status_from_string(required(2)); }
        catch (const std::invalid_argument& e) { throw TaskError("DATABASE_ERROR", e.what()); }
        t.input_path = required(3); t.result_path = optional(4); t.created_at = required(5);
        t.started_at = optional(6); t.finished_at = optional(7);
        t.error_code = optional(8); t.error_message = optional(9);
        return t;
    }
private:
    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, StatementCloser> value_;
};
constexpr const char* columns = "task_id,asset,status,input_path,result_path,created_at,started_at,finished_at,error_code,error_message";
}
struct TaskRepository::Impl {
    std::unique_ptr<sqlite3, DatabaseCloser> db;
    std::mutex mutex;
    explicit Impl(const std::filesystem::path& path) {
        if (!sqlite3_threadsafe()) throw TaskError("DATABASE_ERROR", "SQLite must be built with thread support");
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(path.c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        db.reset(raw);
        check(rc, db.get(), "Open database");
        check(sqlite3_busy_timeout(db.get(), 2000), db.get(), "Set busy timeout");
        check(sqlite3_exec(db.get(), R"sql(
            CREATE TABLE IF NOT EXISTS tasks (
                task_id TEXT PRIMARY KEY,
                asset TEXT NOT NULL,
                status TEXT NOT NULL CHECK(status IN ('QUEUED','RUNNING','SUCCEEDED','FAILED')),
                input_path TEXT NOT NULL,
                result_path TEXT,
                created_at TEXT NOT NULL,
                started_at TEXT,
                finished_at TEXT,
                error_code TEXT,
                error_message TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_tasks_created ON tasks(created_at,task_id);
            CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
        )sql", nullptr, nullptr, nullptr), db.get(), "Create task schema");
    }
};
TaskRepository::TaskRepository(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
TaskRepository::~TaskRepository() = default;

void TaskRepository::insert_queued(const TaskRecord& t) {
    if (t.id.empty() || t.asset.empty() || t.input_path.empty() || t.created_at.empty() ||
        t.status != TaskStatus::Queued || t.result_path || t.started_at || t.finished_at || t.error_code || t.error_message)
        throw TaskError("INVALID_ARGUMENT", "insert_queued requires a fresh task with all required fields");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement s(impl_->db.get(), "INSERT INTO tasks(task_id,asset,status,input_path,created_at) VALUES(?,?,'QUEUED',?,?)");
    s.bind(1,t.id); s.bind(2,t.asset); s.bind(3,t.input_path); s.bind(4,t.created_at);
    s.execute();
}
TaskRecord TaskRepository::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto sql = std::string("SELECT ") + columns + " FROM tasks WHERE task_id=?";
    Statement s(impl_->db.get(), sql.c_str()); s.bind(1,id);
    if (s.step() == SQLITE_DONE) throw TaskError("NOT_FOUND", "Task not found: " + id);
    return s.record();
}
std::vector<TaskRecord> TaskRepository::list(std::size_t limit) {
    if (!limit || limit > 1000) throw TaskError("INVALID_ARGUMENT", "List limit must be 1..1000");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto sql = std::string("SELECT ") + columns + " FROM tasks ORDER BY created_at DESC, task_id DESC LIMIT ?";
    Statement s(impl_->db.get(), sql.c_str()); s.bind_limit(limit);
    std::vector<TaskRecord> rows;
    while (s.step() == SQLITE_ROW) rows.push_back(s.record());
    return rows;
}
bool TaskRepository::mark_running(const std::string& id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement s(impl_->db.get(), "UPDATE tasks SET status='RUNNING',started_at=? WHERE task_id=? AND status='QUEUED'");
    s.bind(1,utc_now()); s.bind(2,id); s.execute();
    return sqlite3_changes(impl_->db.get()) == 1;
}
bool TaskRepository::mark_succeeded(const std::string& id, const std::string& path) {
    if (path.empty()) throw TaskError("INVALID_ARGUMENT", "Result path is empty");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement s(impl_->db.get(), "UPDATE tasks SET status='SUCCEEDED',result_path=?,finished_at=? WHERE task_id=? AND status='RUNNING'");
    s.bind(1,path); s.bind(2,utc_now()); s.bind(3,id); s.execute();
    return sqlite3_changes(impl_->db.get()) == 1;
}
bool TaskRepository::mark_failed(const std::string& id, const std::string& code, const std::string& message) {
    if (code.empty()) throw TaskError("INVALID_ARGUMENT", "Failure code is empty");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement s(impl_->db.get(), "UPDATE tasks SET status='FAILED',error_code=?,error_message=?,finished_at=? WHERE task_id=? AND status='RUNNING'");
    s.bind(1,code); s.bind(2,message); s.bind(3,utc_now()); s.bind(4,id); s.execute();
    return sqlite3_changes(impl_->db.get()) == 1;
}
bool TaskRepository::has_incomplete() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement s(impl_->db.get(), "SELECT 1 FROM tasks WHERE status IN ('QUEUED','RUNNING') LIMIT 1");
    return s.step() == SQLITE_ROW;
}
} // namespace edgeinspect
