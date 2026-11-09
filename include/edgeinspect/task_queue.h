#pragma once
#include "edgeinspect/task.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace edgeinspect {
// Preallocated ring: after before_publish commits, publication cannot allocate.
class TaskQueue {
public:
    explicit TaskQueue(std::size_t capacity);
    bool try_push(QueuedTask task, const std::function<void()>& before_publish = {});
    std::optional<QueuedTask> wait_pop();
    void close();
    std::size_t size() const;
private:
    mutable std::mutex mutex_;
    std::condition_variable readable_;
    std::vector<std::optional<QueuedTask>> slots_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
};
} // namespace edgeinspect
