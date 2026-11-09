#include "edgeinspect/task.h"
#include "edgeinspect/inference_engine.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace edgeinspect {
std::string task_status_to_string(TaskStatus status) {
    switch (status) {
        case TaskStatus::Queued: return "QUEUED";
        case TaskStatus::Running: return "RUNNING";
        case TaskStatus::Succeeded: return "SUCCEEDED";
        case TaskStatus::Failed: return "FAILED";
    }
    throw std::invalid_argument("Invalid TaskStatus");
}
TaskStatus task_status_from_string(const std::string& text) {
    if (text == "QUEUED") return TaskStatus::Queued;
    if (text == "RUNNING") return TaskStatus::Running;
    if (text == "SUCCEEDED") return TaskStatus::Succeeded;
    if (text == "FAILED") return TaskStatus::Failed;
    throw std::invalid_argument("Unknown task status: " + text);
}
std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm tm{};
    if (!gmtime_r(&time, &tm)) throw std::runtime_error("UTC conversion failed");
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << millis << 'Z';
    return out.str();
}
std::string task_record_json(const TaskRecord& t) {
    const auto optional = [](const std::optional<std::string>& x) {
        return x ? json_string(*x) : std::string("null");
    };
    return "{\"task_id\":" + json_string(t.id) + ",\"asset\":" + json_string(t.asset) +
        ",\"status\":" + json_string(task_status_to_string(t.status)) +
        ",\"input_path\":" + json_string(t.input_path) + ",\"result_path\":" + optional(t.result_path) +
        ",\"created_at\":" + json_string(t.created_at) + ",\"started_at\":" + optional(t.started_at) +
        ",\"finished_at\":" + optional(t.finished_at) + ",\"error_code\":" + optional(t.error_code) +
        ",\"error_message\":" + optional(t.error_message) + "}";
}
} // namespace edgeinspect
