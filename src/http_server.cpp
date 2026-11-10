#include "edgeinspect/http_server.h"
#include "httplib.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>

namespace edgeinspect {
namespace {
constexpr std::size_t form_overhead_limit = 16 * 1024;
int error_status(const std::string& code) {
    if (code == "NOT_FOUND") return 404;
    if (code == "NOT_READY" || code == "TASK_FAILED") return 409;
    if (code == "QUEUE_FULL") return 429;
    if (code == "NOT_ACCEPTING" || code == "SERVICE_FAULT") return 503;
    if (code == "PAYLOAD_TOO_LARGE") return 413;
    if (code == "UNSUPPORTED_MEDIA_TYPE") return 415;
    if (code == "INVALID_ARGUMENT" || code == "INVALID_ASSET" || code == "INVALID_INPUT") return 400;
    return 500;
}
void json(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}
void error(httplib::Response& res, int status, const std::string& code, const std::string& message) {
    if (status == 429 || status == 503) res.set_header("Retry-After", "1");
    json(res,status,"{\"error_code\":" + json_string(code) + ",\"error_message\":" + json_string(message) + "}");
}
template<class F> void guarded(httplib::Response& res, F action) {
    try { action(); }
    catch (const TaskError& e) { error(res,error_status(e.code()),e.code(),e.what()); }
    catch (const std::exception& e) {
        std::cerr << "HTTP handler error: " << e.what() << '\n';
        error(res,500,"INTERNAL_ERROR","Request handling failed");
    }
    catch (...) { error(res,500,"INTERNAL_ERROR","Unknown request handling failure"); }
}
void task_id(const std::string& id) {
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw TaskError("INVALID_ARGUMENT","Task ID must be 32 lowercase hexadecimal characters");
}
std::size_t list_limit(const httplib::Request& req) {
    if (req.params.empty()) return 20;
    if (req.params.size() != 1 || !req.has_param("limit"))
        throw TaskError("INVALID_ARGUMENT","Only one limit query parameter is allowed");
    const auto text = req.get_param_value("limit");
    std::size_t limit = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),limit);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || !limit || limit > 1000)
        throw TaskError("INVALID_ARGUMENT","limit must be an integer in 1..1000");
    return limit;
}
void no_query(const httplib::Request& req) {
    if (!req.params.empty()) throw TaskError("INVALID_ARGUMENT","This endpoint does not accept query parameters");
}

// A request-owned upload file. Client filenames are never used as local paths.
class Upload {
public:
    explicit Upload(const std::filesystem::path& directory) {
        auto pattern = (directory / "image-XXXXXX").string();
        fd_ = ::mkstemp(pattern.data());
        if (fd_ < 0) throw TaskError("STORAGE_ERROR","Cannot create upload file");
        // Keep cleanup independent of throwing filesystem path/string allocations.
        try { path_ = pattern; }
        catch (...) { ::close(fd_); ::unlink(pattern.c_str()); fd_ = -1; throw; }
        if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
            ::close(fd_); ::unlink(path_.c_str()); fd_ = -1;
            throw TaskError("STORAGE_ERROR","Cannot set upload close-on-exec flag");
        }
    }
    ~Upload() {
        if (fd_ >= 0) ::close(fd_);
        if (::unlink(path_.c_str()) && errno != ENOENT) std::cerr << "Cannot remove upload file: " << path_ << '\n';
    }
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;
    void append(const char* data, std::size_t size) {
        std::size_t done = 0;
        while (done < size) {
            const auto n = ::write(fd_,data+done,size-done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw TaskError("STORAGE_ERROR","Cannot write upload file");
            done += static_cast<std::size_t>(n);
        }
    }
    void finish() {
        const int fd = fd_; fd_ = -1;
        if (::close(fd)) throw TaskError("STORAGE_ERROR","Cannot close upload file");
    }
    const std::filesystem::path& path() const { return path_; }
private:
    int fd_ = -1;
    std::filesystem::path path_;
};
class UploadDirectory {
public:
    explicit UploadDirectory(const std::filesystem::path& root) {
        auto pattern = (std::filesystem::canonical(root) / ".http-uploads-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw TaskError("STORAGE_ERROR","Cannot create upload directory");
        try { path = pattern; }
        catch (...) { ::rmdir(pattern.c_str()); throw; }
    }
    ~UploadDirectory() {
        std::error_code ignored; std::filesystem::remove_all(path,ignored);
        if (ignored) std::cerr << "Cannot remove upload directory: " << ignored.message() << '\n';
    }
    std::filesystem::path path;
};
}
struct HttpServer::Impl {
    TaskManager& manager;
    HttpServerConfig config;
    UploadDirectory uploads; // Server must stop/join handlers before this dies.
    httplib::Server server;
    int bound_port = -1;

    Impl(TaskManager& m, HttpServerConfig c)
        : manager(m), config(std::move(c)), uploads(config.data_root) {
        if (!config.max_image_bytes || config.max_image_bytes > std::numeric_limits<std::size_t>::max()-form_overhead_limit)
            throw TaskError("INVALID_ARGUMENT","Invalid HTTP image byte limit");
        if (config.port < 0 || config.port > 65535) throw TaskError("INVALID_ARGUMENT","port must be 0..65535");
        // HTTP workers handle I/O; only TaskManager's one worker touches the engine.
        server.new_task_queue = [] { return new httplib::ThreadPool(4,4,16); };
        // Do not inherit httplib's Linux SO_REUSEPORT default: separate services
        // must not silently share the same port and split requests/databases.
        server.set_socket_options([](auto socket) {
            const int enabled = 1;
            ::setsockopt(socket,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled));
        });
        server.set_payload_max_length(config.max_image_bytes + form_overhead_limit);
        server.set_read_timeout(5); server.set_write_timeout(5);
        server.set_keep_alive_timeout(2); server.set_keep_alive_max_count(10);
        server.set_default_headers({{"Cache-Control","no-store"},{"X-Content-Type-Options","nosniff"}});
        server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty()) return;
            const auto code = res.status == 413 ? "PAYLOAD_TOO_LARGE" : res.status == 404 ? "NOT_FOUND" : "HTTP_ERROR";
            error(res,res.status,code,"HTTP request rejected");
        });
        server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            error(res,500,"INTERNAL_ERROR","Unhandled HTTP transport error");
        });
        server.Get("/health",[this](const auto& req, auto& res) {
            guarded(res,[&] {
                no_query(req);
                if (auto fault = manager.fault()) throw TaskError("SERVICE_FAULT",*fault);
                json(res,200,"{\"status\":\"READY\",\"engine_initializations\":1}");
            });
        });
        server.Get("/tasks",[this](const auto& req, auto& res) {
            guarded(res,[&] {
                const auto rows = manager.list(list_limit(req));
                std::string body = "{\"tasks\":[";
                for (std::size_t i = 0; i < rows.size(); ++i) { if (i) body += ','; body += task_record_json(rows[i]); }
                json(res,200,body + "]}");
            });
        });
        server.Get(R"(/tasks/([^/]+)/result)",[this](const auto& req, auto& res) {
            guarded(res,[&] {
                no_query(req);
                const auto id = req.matches[1].str(); task_id(id);
                const auto task = manager.get(id);
                if (task.status == TaskStatus::Failed) throw TaskError("TASK_FAILED","Task failed; query its status for error details");
                json(res,200,manager.result(id));
            });
        });
        server.Get(R"(/tasks/([^/]+))",[this](const auto& req, auto& res) {
            guarded(res,[&] {
                no_query(req);
                const auto id = req.matches[1].str(); task_id(id);
                json(res,200,task_record_json(manager.get(id)));
            });
        });
        server.Post("/tasks",[this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
            // Rejected streaming bodies may remain unread; do not reuse this connection.
            res.set_header("Connection","close");
            guarded(res,[&] { submit(req,res,reader); });
        });
    }
    void submit(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
        no_query(req);
        if (manager.fault()) throw TaskError("NOT_ACCEPTING","Task service has a fault");
        if (!req.is_multipart_form_data() || (req.has_header("Content-Encoding") && req.get_header_value("Content-Encoding") != "identity"))
            throw TaskError("UNSUPPORTED_MEDIA_TYPE","Use uncompressed multipart/form-data with image and asset fields");
        Upload upload(uploads.path);
        bool image_seen = false, asset_seen = false;
        std::string field, asset;
        std::size_t image_bytes = 0, parts = 0;
        std::exception_ptr failure;
        const auto protect = [&](auto action) {
            if (failure) return false;
            try { action(); return true; }
            catch (...) { failure = std::current_exception(); return false; }
        };
        const bool complete = reader(
            [&](const httplib::FormData& part) {
                return protect([&] {
                    if (++parts > 2) throw TaskError("INVALID_ARGUMENT","Exactly one image and one asset field are required");
                    field = part.name;
                    if (field == "image" && !part.filename.empty() && !image_seen) image_seen = true;
                    else if (field == "asset" && part.filename.empty() && !asset_seen) asset_seen = true;
                    else throw TaskError("INVALID_ARGUMENT","Unknown, duplicate, or invalid multipart field");
                });
            },
            [&](const char* data, std::size_t length) {
                return protect([&] {
                    if (field == "image") {
                        if (length > config.max_image_bytes-image_bytes) throw TaskError("PAYLOAD_TOO_LARGE","Image exceeds max-bytes");
                        upload.append(data,length); image_bytes += length;
                    } else if (field == "asset") {
                        if (length > 128-asset.size()) throw TaskError("INVALID_ASSET","Asset name exceeds 128 bytes");
                        asset.append(data,length);
                    } else throw TaskError("INVALID_ARGUMENT","Multipart content without a field");
                });
            });
        if (failure) std::rethrow_exception(failure);
        if (!complete) {
            const int status = res.status >= 400 ? res.status : 400;
            error(res,status,status == 413 ? "PAYLOAD_TOO_LARGE" : "INVALID_ARGUMENT","Incomplete or malformed upload");
            return;
        }
        if (!image_seen || !asset_seen || !image_bytes || asset.empty())
            throw TaskError("INVALID_ARGUMENT","A nonempty image file and asset field are required");
        upload.finish();
        const auto id = manager.submit(upload.path(),asset);
        const auto status_url = "/tasks/" + id;
        res.set_header("Location",status_url);
        json(res,202,"{\"task_id\":" + json_string(id) + ",\"status_url\":" + json_string(status_url) +
                     ",\"result_url\":" + json_string(status_url + "/result") + "}");
    }
};
HttpServer::HttpServer(TaskManager& manager, HttpServerConfig config)
    : impl_(std::make_unique<Impl>(manager,std::move(config))) {}
HttpServer::~HttpServer() { stop(); }
int HttpServer::bind() {
    if (impl_->bound_port >= 0) throw TaskError("INVALID_ARGUMENT","HTTP server is already bound");
    if (impl_->config.port == 0) impl_->bound_port = impl_->server.bind_to_any_port("127.0.0.1");
    else if (impl_->server.bind_to_port("127.0.0.1",impl_->config.port)) impl_->bound_port = impl_->config.port;
    if (impl_->bound_port < 0) throw TaskError("BIND_ERROR","Cannot bind HTTP listener to 127.0.0.1");
    return impl_->bound_port;
}
bool HttpServer::listen() {
    if (impl_->bound_port < 0) throw TaskError("INVALID_ARGUMENT","Call bind before listen");
    return impl_->server.listen_after_bind();
}
bool HttpServer::is_running() const { return impl_->server.is_running(); }
void HttpServer::stop() noexcept { impl_->server.stop(); }
} // namespace edgeinspect
