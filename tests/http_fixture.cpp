// Only tests inject this processor. edgeinspect_http always constructs RKNN.
#include "edgeinspect/http_server.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    using namespace edgeinspect;
    try {
        if (argc < 2 || argc > 5) return 1;
        TaskManagerConfig config;
        config.data_root = argv[1];
        config.queue_capacity = argc > 2 ? std::stoul(argv[2]) : 1;
        config.inference.max_image_bytes = argc > 3 ? std::stoul(argv[3]) : 1024;
        HttpServerConfig http;
        http.data_root = config.data_root; http.port = argc > 4 ? std::stoi(argv[4]) : 0;
        http.max_image_bytes = config.inference.max_image_bytes;
        std::atomic<int> starts{0}, calls{0}, active{0}, maximum{0};
        TaskManager manager(config,[&] {
            ++starts;
            return TaskProcessor([&](const std::filesystem::path& path,const std::string& asset) {
                ++calls; const int current = ++active;
                maximum.store(std::max(maximum.load(),current));
                struct Exit { std::atomic<int>& n; ~Exit() { --n; } } exit{active};
                std::ifstream in(path,std::ios::binary);
                const std::string body{std::istreambuf_iterator<char>(in),{}};
                if (body == "hold") {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    while (!std::filesystem::exists(config.data_root / "release")) {
                        if (std::chrono::steady_clock::now() >= until) throw TaskError("INFERENCE_ERROR","Test gate timed out");
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                if (body == "bad") throw TaskError("INVALID_INPUT","Broken test image");
                if (body == "fatal") throw TaskError("INFERENCE_ERROR","Injected device failure");
                return "{\"score\":0.25,\"asset\":" + json_string(asset) +
                       ",\"is_anomaly\":null,\"anomaly_threshold\":null}";
            });
        });
        const int result = run_http_service(manager,http);
        std::ofstream out(config.data_root / "fixture-counts.json");
        out << "{\"initializations\":" << starts << ",\"calls\":" << calls << ",\"max_active\":" << maximum << "}";
        return result;
    } catch (const TaskError& e) {
        std::cerr << "{\"error_code\":" << json_string(e.code()) << ",\"error_message\":" << json_string(e.what()) << "}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 2;
}
