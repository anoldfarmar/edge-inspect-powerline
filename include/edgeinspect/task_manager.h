#pragma once
#include "edgeinspect/task.h"
#include "edgeinspect/inference_engine.h"
#include <functional>
#include <memory>
#include <vector>

namespace edgeinspect {
struct TaskManagerConfig {
    Config inference;
    std::filesystem::path data_root;
    std::size_t queue_capacity = 10;
};
// A small injection seam for deterministic tests. Production uses RKNN.
using TaskProcessor = std::function<std::string(const std::filesystem::path&, const std::string&)>;
using ProcessorFactory = std::function<TaskProcessor()>;

class TaskManager {
public:
    explicit TaskManager(TaskManagerConfig config, ProcessorFactory factory = {});
    ~TaskManager();
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    // start/stop are lifecycle operations; do not invoke them from the processor.
    void start();
    std::string submit(const std::filesystem::path& image, const std::string& asset);
    TaskRecord get(const std::string& id);
    std::vector<TaskRecord> list(std::size_t limit = 20);
    std::string result(const std::string& id);
    void stop_and_drain();
    std::optional<std::string> fault() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edgeinspect
