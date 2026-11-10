#pragma once
#include "edgeinspect/task_manager.h"
#include <memory>

namespace edgeinspect {
struct HttpServerConfig {
    std::filesystem::path data_root;
    std::size_t max_image_bytes = 32 * 1024 * 1024;
    int port = 8080; // 0 chooses an available port, useful in automated tests.
};

// Transport adapter only. It borrows a live TaskManager; never calls infer().
class HttpServer {
public:
    HttpServer(TaskManager&, HttpServerConfig);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    int bind();                      // Always IPv4 loopback 127.0.0.1.
    bool listen();                   // Blocking; call after bind().
    bool is_running() const;
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Starts manager, serves until SIGINT/SIGTERM, stops HTTP, then drains tasks.
// Call from the controlling thread before starting any other application threads.
int run_http_service(TaskManager&, const HttpServerConfig&);
} // namespace edgeinspect
