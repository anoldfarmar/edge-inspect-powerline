#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace edgeinspect {
enum class TaskStatus { Queued, Running, Succeeded, Failed };
std::string task_status_to_string(TaskStatus);
TaskStatus task_status_from_string(const std::string&);

// Codes describe execution errors, never whether the image is anomalous.
class TaskError : public std::runtime_error {
public:
    TaskError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

struct TaskRecord {
    std::string id, asset;
    TaskStatus status = TaskStatus::Queued;
    std::string input_path; // Relative to FileStorage::root().
    std::optional<std::string> result_path;
    std::string created_at;
    std::optional<std::string> started_at, finished_at;
    std::optional<std::string> error_code, error_message;
};
struct QueuedTask {
    std::string id, asset, input_path;
};
std::string utc_now();
std::string task_record_json(const TaskRecord&);
} // namespace edgeinspect
