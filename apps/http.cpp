#include "edgeinspect/http_server.h"
#include <charconv>
#include <iostream>
#include <map>

namespace {
std::uint64_t number(const std::string& text, bool allow_zero = false) {
    std::uint64_t n = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),n);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || (!allow_zero && !n))
        throw edgeinspect::TaskError("INVALID_ARGUMENT","Invalid integer: " + text);
    return n;
}
void help() {
    std::cout << "edgeinspect_http --root PROJECT --data-root DIRECTORY [--port 8080]\n"
              << "  [--queue-capacity 10] [--max-bytes 33554432] [--max-pixels 24000000]\n"
              << "Loopback only: 127.0.0.1. port=0 chooses an available port.\n"
              << "POST /tasks: multipart image file + asset field; returns 202 + task ID.\n"
              << "GET /health | /tasks?limit=20 | /tasks/ID | /tasks/ID/result\n"
              << "SIGINT/SIGTERM stops HTTP then drains accepted tasks. No video/recovery.\n";
}
}
int main(int argc, char** argv) {
    using namespace edgeinspect;
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { help(); return 0; }
        std::map<std::string,std::string> args;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key != "--root" && key != "--data-root" && key != "--port" && key != "--queue-capacity" &&
                key != "--max-bytes" && key != "--max-pixels") throw TaskError("INVALID_ARGUMENT","Unknown option: " + key);
            if (++i == argc || args.count(key)) throw TaskError("INVALID_ARGUMENT","Missing or duplicate option: " + key);
            args.emplace(key,argv[i]);
        }
        if (!args.count("--data-root")) throw TaskError("INVALID_ARGUMENT","--data-root is required");
        TaskManagerConfig tasks;
        tasks.inference.root = args.count("--root") ? std::filesystem::path(args.at("--root")) : std::filesystem::current_path();
        tasks.data_root = args.at("--data-root");
        HttpServerConfig http;
        http.data_root = tasks.data_root;
        if (args.count("--port")) {
            const auto port = number(args.at("--port"),true);
            if (port > 65535) throw TaskError("INVALID_ARGUMENT","port exceeds 65535");
            http.port = static_cast<int>(port);
        }
        if (args.count("--queue-capacity")) {
            const auto capacity = number(args.at("--queue-capacity"));
            if (capacity > 100000) throw TaskError("INVALID_ARGUMENT","queue-capacity exceeds 100000");
            tasks.queue_capacity = capacity;
        }
        if (args.count("--max-bytes")) tasks.inference.max_image_bytes = number(args.at("--max-bytes"));
        if (args.count("--max-pixels")) tasks.inference.max_image_pixels = number(args.at("--max-pixels"));
        http.max_image_bytes = tasks.inference.max_image_bytes;
        TaskManager manager(tasks);
        return run_http_service(manager,http);
    } catch (const TaskError& e) {
        std::cerr << "{\"error_code\":" << json_string(e.code()) << ",\"error_message\":" << json_string(e.what()) << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "{\"error_code\":\"STARTUP_ERROR\",\"error_message\":" << json_string(e.what()) << "}\n";
    }
    return 2;
}
