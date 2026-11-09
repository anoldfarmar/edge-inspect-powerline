#include "edgeinspect/file_storage.h"
#include "edgeinspect/inference_engine.h"
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <exception>

namespace edgeinspect {
namespace {
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void finish() {
        const int fd = value; value = -1;
        if (::close(fd)) throw TaskError("STORAGE_ERROR", "Close file: " + std::string(std::strerror(errno)));
    }
};
[[noreturn]] void storage_error(const std::string& message) {
    throw TaskError("STORAGE_ERROR", message + ": " + std::strerror(errno));
}
bool valid_id(const std::string& id) {
    if (id.size() != 32) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
void check_id(const std::string& id) {
    if (!valid_id(id)) throw TaskError("INVALID_ARGUMENT", "Invalid storage task ID");
}
std::string random_id() {
    std::array<unsigned char,16> bytes{};
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::getrandom(bytes.data() + done, bytes.size() - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) storage_error("Generate task ID");
        done += static_cast<std::size_t>(n);
    }
    const char* hex = "0123456789abcdef";
    std::string id; id.reserve(32);
    for (auto x : bytes) { id.push_back(hex[x >> 4]); id.push_back(hex[x & 15]); }
    return id;
}
void write_all(int fd, const char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto n = ::write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) storage_error("Write input copy");
        done += static_cast<std::size_t>(n);
    }
}
}
struct FileStorage::Impl {
    std::filesystem::path root;
    Fd lock;
    explicit Impl(const std::filesystem::path& path) {
        if (path.empty()) throw TaskError("INVALID_ARGUMENT", "data_root is empty");
        std::filesystem::create_directories(path);
        root = std::filesystem::canonical(path);
        lock.value = ::open((root / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock.value < 0) storage_error("Open data-root lock");
        if (::flock(lock.value, LOCK_EX | LOCK_NB)) {
            if (errno == EWOULDBLOCK) throw TaskError("ALREADY_RUNNING", "Another process owns this data_root");
            storage_error("Lock data_root");
        }
        const auto tasks = root / "tasks";
        if (std::filesystem::is_symlink(tasks)) throw TaskError("STORAGE_ERROR", "tasks directory cannot be a symlink");
        std::filesystem::create_directories(tasks);
        if (std::filesystem::is_symlink(root / "tasks.sqlite3"))
            throw TaskError("STORAGE_ERROR", "Database cannot be a symlink");
    }
};
FileStorage::FileStorage(const std::filesystem::path& root) {
    try { impl_ = std::make_unique<Impl>(root); }
    catch (const std::filesystem::filesystem_error& e) { throw TaskError("STORAGE_ERROR", e.what()); }
}
FileStorage::~FileStorage() = default;
const std::filesystem::path& FileStorage::root() const { return impl_->root; }
std::string FileStorage::input_relative(const std::string& id) const { check_id(id); return "tasks/" + id + "/input.bin"; }
std::string FileStorage::result_relative(const std::string& id) const { check_id(id); return "tasks/" + id + "/result.json"; }
std::filesystem::path FileStorage::resolve(const std::string& relative) const {
    const std::filesystem::path p(relative);
    const auto id = p.parent_path().filename().string();
    check_id(id);
    if (relative != input_relative(id) && relative != result_relative(id))
        throw TaskError("INVALID_ARGUMENT", "Not a controlled task path");
    try {
        auto current = impl_->root;
        for (const auto& component : p) {
            current /= component;
            if (std::filesystem::is_symlink(current)) throw TaskError("STORAGE_ERROR", "Task path contains a symlink");
        }
        return current;
    } catch (const std::filesystem::filesystem_error& e) { throw TaskError("STORAGE_ERROR", e.what()); }
}
std::string FileStorage::copy_input(const std::filesystem::path& source, std::uintmax_t max_bytes) {
    if (!max_bytes) throw TaskError("INVALID_ARGUMENT", "Image byte limit must be positive");
    // O_NONBLOCK avoids hanging on a FIFO before fstat can reject it.
    Fd input(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (input.value < 0) throw TaskError("INVALID_INPUT", "Cannot open source image: " + source.string());
    struct stat info{};
    if (::fstat(input.value, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uintmax_t>(info.st_size) > max_bytes)
        throw TaskError("INVALID_INPUT", "Input must be a nonempty regular file within the byte limit");
    std::string id;
    try {
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto candidate = random_id();
            if (std::filesystem::create_directory(impl_->root / "tasks" / candidate)) {
                id = std::move(candidate); break;
            }
        }
        if (id.empty()) throw TaskError("STORAGE_ERROR", "Task ID collisions");
        const auto target = resolve(input_relative(id));
        Fd output(::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (output.value < 0) storage_error("Create input copy");
        std::array<char,65536> buffer{};
        std::uintmax_t total = 0;
        for (;;) {
            const auto n = ::read(input.value, buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw TaskError("INVALID_INPUT", "Cannot read source image");
            if (!n) break;
            const auto bytes = static_cast<std::size_t>(n);
            if (bytes > max_bytes - total) throw TaskError("INVALID_INPUT", "Source grew beyond byte limit");
            write_all(output.value, buffer.data(), bytes); total += bytes;
        }
        if (!total) throw TaskError("INVALID_INPUT", "Source became empty");
        output.finish();
        return id;
    } catch (...) {
        const auto original = std::current_exception();
        if (!id.empty()) remove_task(id);
        try { std::rethrow_exception(original); }
        catch (const std::filesystem::filesystem_error& e) { throw TaskError("STORAGE_ERROR", e.what()); }
    }
}
void FileStorage::remove_task(const std::string& id) {
    check_id(id);
    try { std::filesystem::remove_all(impl_->root / "tasks" / id); }
    catch (const std::filesystem::filesystem_error& e) { throw TaskError("STORAGE_ERROR", "Input cleanup failed: " + std::string(e.what())); }
}
void FileStorage::write_result(const std::string& id, const std::string& json) {
    const auto path = resolve(result_relative(id));
    try { save_new_file(path, json.data(), json.size()); }
    catch (const std::exception& e) { throw TaskError("STORAGE_ERROR", e.what()); }
}
std::string FileStorage::read_result(const std::string& relative) const {
    const auto path = resolve(relative);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw TaskError("STORAGE_ERROR", "Cannot read result");
    const auto size = in.tellg();
    if (size < 0 || size > 4 * 1024 * 1024) throw TaskError("STORAGE_ERROR", "Result file exceeds 4 MiB limit");
    std::string text(static_cast<std::size_t>(size), '\0'); in.seekg(0);
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) throw TaskError("STORAGE_ERROR", "Read result failed");
    return text;
}
} // namespace edgeinspect
