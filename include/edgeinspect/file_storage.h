#pragma once
#include "edgeinspect/task.h"
#include <filesystem>
#include <memory>

namespace edgeinspect {
class FileStorage {
public:
    explicit FileStorage(const std::filesystem::path& root);
    ~FileStorage();
    FileStorage(const FileStorage&) = delete;
    FileStorage& operator=(const FileStorage&) = delete;
    const std::filesystem::path& root() const;
    std::string copy_input(const std::filesystem::path& source, std::uintmax_t max_bytes);
    void remove_task(const std::string& id); // Only for submissions not published.
    std::string input_relative(const std::string& id) const;
    std::string result_relative(const std::string& id) const;
    std::filesystem::path resolve(const std::string& relative) const;
    void write_result(const std::string& id, const std::string& json);
    std::string read_result(const std::string& relative) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace edgeinspect
