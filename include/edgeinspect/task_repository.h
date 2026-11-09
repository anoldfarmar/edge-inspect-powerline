#pragma once
#include "edgeinspect/task.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace edgeinspect {
// One connection; each entire operation is protected by an internal mutex.
class TaskRepository {
public:
    explicit TaskRepository(const std::filesystem::path& database_path);
    ~TaskRepository();
    TaskRepository(const TaskRepository&) = delete;
    TaskRepository& operator=(const TaskRepository&) = delete;
    void insert_queued(const TaskRecord&);
    TaskRecord get(const std::string& id);
    std::vector<TaskRecord> list(std::size_t limit = 20);
    bool mark_running(const std::string& id);
    bool mark_succeeded(const std::string& id, const std::string& result_path);
    bool mark_failed(const std::string& id, const std::string& code, const std::string& message);
    bool has_incomplete();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edgeinspect
