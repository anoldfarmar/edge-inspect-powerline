#include "edgeinspect/http_server.h"
#include <pthread.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <iostream>
#include <thread>

namespace edgeinspect {
namespace {
class Signals {
public:
    Signals() {
        sigemptyset(&set); sigaddset(&set,SIGINT); sigaddset(&set,SIGTERM);
        if (pthread_sigmask(SIG_BLOCK,&set,&previous)) throw TaskError("STARTUP_ERROR","Cannot block shutdown signals");
    }
    ~Signals() { pthread_sigmask(SIG_SETMASK,&previous,nullptr); }
    bool stop_requested() {
        const timespec timeout{0,200000000};
        const int signal = sigtimedwait(&set,nullptr,&timeout);
        if (signal == SIGINT || signal == SIGTERM) return true;
        if (signal < 0 && errno != EAGAIN && errno != EINTR) throw TaskError("SIGNAL_ERROR","Cannot wait for shutdown signal");
        return false;
    }
private:
    sigset_t set{}, previous{};
};
}
int run_http_service(TaskManager& manager, const HttpServerConfig& config) {
    // Block before manager.start(): the inference and HTTP threads inherit the mask.
    // Signals are consumed synchronously here, never via an unsafe signal handler.
    Signals signals;
    HttpServer server(manager,config);
    const int port = server.bind();
    manager.start();
    std::atomic<bool> finished{false};
    bool listen_ok = false;
    std::exception_ptr listener_error;
    std::thread listener([&] {
        try { listen_ok = server.listen(); }
        catch (...) { listener_error = std::current_exception(); }
        finished.store(true);
    });
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!server.is_running() && !finished.load()) {
            if (std::chrono::steady_clock::now() >= deadline) throw TaskError("STARTUP_ERROR","HTTP startup timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (finished.load()) throw TaskError("STARTUP_ERROR","HTTP listener exited during startup");
        std::cout << "{\"event\":\"HTTP_READY\",\"listen\":\"127.0.0.1\",\"port\":" << port
                  << ",\"engine_initializations\":1}\n" << std::flush;
        while (!finished.load() && !signals.stop_requested()) {}
        server.stop();
        listener.join(); // Finish/reject HTTP handlers before stopping task admission.
    } catch (...) {
        server.stop(); listener.join(); manager.stop_and_drain(); throw;
    }
    manager.stop_and_drain();
    if (listener_error) std::rethrow_exception(listener_error);
    if (!listen_ok) throw TaskError("HTTP_ERROR","HTTP listener failed");
    if (auto fault = manager.fault()) throw TaskError("SERVICE_FAULT",*fault);
    std::cout << "{\"event\":\"HTTP_STOPPED\"}\n" << std::flush;
    return 0;
}
} // namespace edgeinspect
