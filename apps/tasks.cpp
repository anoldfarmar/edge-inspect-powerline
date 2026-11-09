#include "edgeinspect/task_manager.h"
#include <unistd.h>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::uint64_t positive(const std::string& text) {
    std::uint64_t n = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !n)
        throw edgeinspect::TaskError("INVALID_ARGUMENT", "Expected positive integer: " + text);
    return n;
}
std::map<std::string,std::string> options(const std::vector<std::string>& words, std::size_t first,
                                         const std::vector<std::string>& allowed) {
    std::map<std::string,std::string> result;
    for (std::size_t i = first; i < words.size(); i += 2) {
        const auto& key = words[i];
        bool known = false;
        for (const auto& x : allowed) if (key == x) known = true;
        if (!known || i + 1 == words.size() || result.count(key))
            throw edgeinspect::TaskError("INVALID_ARGUMENT", "Unknown, duplicate, or missing option: " + key);
        result.emplace(key, words[i + 1]);
    }
    return result;
}
void help() {
    std::cout << "Start: edgeinspect_tasks --root PROJECT --data-root DIRECTORY [--queue-capacity 10]\n"
        "       [--max-bytes 33554432] [--max-pixels 24000000]\n"
        "Commands (double-quote paths containing spaces):\n"
        "  submit --image PATH --asset ASSET\n"
        "  get ID | result ID | wait ID [TIMEOUT_MS]\n"
        "  list [LIMIT] | health | help | quit\n"
        "wait defaults to 30000 ms; quit/EOF drains accepted tasks. No HTTP/video/recovery.\n";
}
void error(const std::string& code, const std::string& message) {
    std::cout << "{\"error_code\":" << edgeinspect::json_string(code)
              << ",\"error_message\":" << edgeinspect::json_string(message) << "}\n" << std::flush;
}
}
int main(int argc, char** argv) {
    using namespace edgeinspect;
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        if (arguments.size() == 1 && arguments[0] == "--help") { help(); return 0; }
        const auto args = options(arguments, 0, {"--root","--data-root","--queue-capacity","--max-bytes","--max-pixels"});
        if (!args.count("--data-root")) throw TaskError("INVALID_ARGUMENT", "--data-root is required");
        TaskManagerConfig config;
        config.inference.root = args.count("--root") ? std::filesystem::path(args.at("--root")) : std::filesystem::current_path();
        config.data_root = args.at("--data-root");
        if (args.count("--queue-capacity")) {
            const auto n = positive(args.at("--queue-capacity"));
            if (n > 100000) throw TaskError("INVALID_ARGUMENT", "queue-capacity exceeds 100000");
            config.queue_capacity = n;
        }
        if (args.count("--max-bytes")) config.inference.max_image_bytes = positive(args.at("--max-bytes"));
        if (args.count("--max-pixels")) config.inference.max_image_pixels = positive(args.at("--max-pixels"));
        TaskManager manager(config);
        manager.start();
        std::cout << "{\"event\":\"READY\",\"engine_initializations\":1,\"data_root\":"
                  << json_string(std::filesystem::canonical(config.data_root).string()) << "}\n" << std::flush;
        std::string line;
        const bool interactive = ::isatty(STDIN_FILENO);
        while (true) {
            if (interactive) std::cerr << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            try {
                std::istringstream input(line);
                std::vector<std::string> words;
                while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
                    std::string word;
                    if (!(input >> std::quoted(word))) throw TaskError("INVALID_ARGUMENT", "Unclosed quoted argument");
                    words.push_back(std::move(word));
                }
                if (words.empty()) continue;
                const auto& command = words[0];
                if (command == "quit" && words.size() == 1) break;
                if (command == "help" && words.size() == 1) { help(); continue; }
                if (command == "submit") {
                    const auto flags = options(words, 1, {"--image","--asset"});
                    if (flags.size() != 2) throw TaskError("INVALID_ARGUMENT", "submit requires --image and --asset");
                    const auto id = manager.submit(flags.at("--image"), flags.at("--asset"));
                    std::cout << "{\"event\":\"ACCEPTED\",\"task_id\":" << json_string(id) << "}\n";
                } else if (command == "get" && words.size() == 2) {
                    std::cout << task_record_json(manager.get(words[1])) << '\n';
                } else if (command == "result" && words.size() == 2) {
                    std::cout << manager.result(words[1]) << '\n';
                } else if (command == "list" && words.size() <= 2) {
                    const auto rows = manager.list(words.size() == 2 ? positive(words[1]) : 20);
                    std::cout << '[';
                    for (std::size_t i = 0; i < rows.size(); ++i) { if (i) std::cout << ','; std::cout << task_record_json(rows[i]); }
                    std::cout << "]\n";
                } else if (command == "wait" && (words.size() == 2 || words.size() == 3)) {
                    const auto timeout = words.size() == 3 ? positive(words[2]) : 30000;
                    if (timeout > 600000) throw TaskError("INVALID_ARGUMENT", "wait timeout exceeds 600000 ms");
                    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
                    for (;;) {
                        const auto t = manager.get(words[1]);
                        if (t.status == TaskStatus::Succeeded || t.status == TaskStatus::Failed) {
                            std::cout << task_record_json(t) << '\n'; break;
                        }
                        if (auto fault = manager.fault()) throw TaskError("SERVICE_FAULT", *fault);
                        if (std::chrono::steady_clock::now() >= until) throw TaskError("WAIT_TIMEOUT", "Task is still pending");
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } else if (command == "health" && words.size() == 1) {
                    const auto fault = manager.fault();
                    std::cout << "{\"fault\":" << (fault ? json_string(*fault) : "null") << "}\n";
                } else throw TaskError("INVALID_ARGUMENT", "Unknown command or argument count; use help");
            } catch (const TaskError& e) { error(e.code(), e.what()); }
              catch (const std::exception& e) { error("COMMAND_ERROR", e.what()); }
            std::cout << std::flush;
        }
        manager.stop_and_drain();
        if (const auto fault = manager.fault()) { error("SERVICE_FAULT", *fault); return 2; }
        std::cout << "{\"event\":\"STOPPED\"}\n";
        return 0;
    } catch (const TaskError& e) { error(e.code(), e.what()); return 2; }
      catch (const std::exception& e) { error("STARTUP_ERROR", e.what()); return 2; }
}
