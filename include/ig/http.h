// Minimal blocking HTTP/1.1 server + client. ~250 lines, no deps.
//
// Production deployments would use cpp-httplib or boost::beast; this
// implementation exists so the project compiles with zero external
// libraries and so the gateway is fully self-contained for demos and
// CI. The router/scheduler/metrics don't depend on it.
#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ig {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

// RawHandler is the streaming-mode handler. It is given the live
// client fd; the handler is responsible for writing the entire HTTP
// response (status line + headers + body chunks). After the handler
// returns the connection is closed by the server. Use this for SSE
// pass-through; ordinary JSON endpoints should use Handler.
//
// Returning false signals an unrecoverable I/O error so the server
// can stop trying to write to the fd.
struct RawContext {
    int fd = -1;
    HttpRequest req;
};

class HttpServer {
public:
    using Handler    = std::function<HttpResponse(const HttpRequest&)>;
    using RawHandler = std::function<bool(const RawContext&)>;

    void Route(const std::string& method, const std::string& path, Handler h) {
        std::lock_guard<std::mutex> lk(mu_);
        routes_[method + " " + path] = std::move(h);
    }

    // RouteRaw registers a streaming handler. Wins over Route on
    // collisions (lookup checks raw_routes_ first).
    void RouteRaw(const std::string& method, const std::string& path, RawHandler h) {
        std::lock_guard<std::mutex> lk(mu_);
        raw_routes_[method + " " + path] = std::move(h);
    }

    bool Listen(const std::string& host, int port, int worker_threads = 32) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = host == "0.0.0.0" || host.empty()
            ? INADDR_ANY
            : ::inet_addr(host.c_str());

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(listen_fd_);
            return false;
        }
        if (::listen(listen_fd_, 128) < 0) {
            ::close(listen_fd_);
            return false;
        }
        running_.store(true);
        for (int i = 0; i < worker_threads; ++i) {
            workers_.emplace_back(&HttpServer::workerLoop, this);
        }
        return true;
    }

    void Stop() {
        running_.store(false);
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR), ::close(listen_fd_);
        for (auto& t : workers_) if (t.joinable()) t.join();
        workers_.clear();
    }

    ~HttpServer() { Stop(); }

private:
    void workerLoop() {
        while (running_.load()) {
            sockaddr_in cli{};
            socklen_t sz = sizeof(cli);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &sz);
            if (fd < 0) {
                if (!running_.load()) return;
                continue;
            }
            handleConn(fd);
            ::close(fd);
        }
    }

    void handleConn(int fd) {
        std::string buf;
        char tmp[4096];
        while (true) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, n);
            auto h = buf.find("\r\n\r\n");
            if (h == std::string::npos) continue;
            HttpRequest req;
            if (!parseRequest(buf, req)) {
                sendResponse(fd, {.status = 400, .content_type = "text/plain", .body = "bad request"});
                return;
            }
            // Naive Content-Length handling.
            size_t cl = 0;
            auto it = req.headers.find("content-length");
            if (it != req.headers.end()) cl = std::stoul(it->second);
            size_t body_have = buf.size() - h - 4;
            while (body_have < cl) {
                ssize_t m = ::recv(fd, tmp, sizeof(tmp), 0);
                if (m <= 0) return;
                buf.append(tmp, m);
                body_have += m;
            }
            req.body = buf.substr(h + 4, cl);
            // Raw (streaming) handlers win over normal handlers — they
            // get exclusive write access to the fd.
            RawHandler raw;
            Handler hnd;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto rt = raw_routes_.find(req.method + " " + req.path);
                if (rt != raw_routes_.end()) raw = rt->second;
                else {
                    auto rit = routes_.find(req.method + " " + req.path);
                    if (rit != routes_.end()) hnd = rit->second;
                }
            }
            if (raw) {
                RawContext ctx{fd, std::move(req)};
                (void)raw(ctx);
                return;
            }
            HttpResponse resp;
            if (!hnd) {
                resp = {.status = 404, .content_type = "text/plain", .body = "not found"};
            } else {
                resp = hnd(req);
            }
            sendResponse(fd, resp);
            return;  // one request per connection, simplifies things
        }
    }

    static bool parseRequest(const std::string& buf, HttpRequest& out) {
        auto eol = buf.find("\r\n");
        if (eol == std::string::npos) return false;
        std::istringstream rl(buf.substr(0, eol));
        std::string version;
        if (!(rl >> out.method >> out.path >> version)) return false;
        size_t pos = eol + 2;
        while (pos < buf.size()) {
            auto e = buf.find("\r\n", pos);
            if (e == std::string::npos) break;
            if (e == pos) break;  // end of headers
            std::string line = buf.substr(pos, e - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                for (auto& c : k) c = static_cast<char>(::tolower(c));
                out.headers[k] = v;
            }
            pos = e + 2;
        }
        return true;
    }

    static void sendResponse(int fd, const HttpResponse& r) {
        std::ostringstream os;
        os << "HTTP/1.1 " << r.status << " "
           << (r.status == 200 ? "OK" : r.status == 404 ? "Not Found" : "Status") << "\r\n"
           << "Content-Type: " << r.content_type << "\r\n"
           << "Content-Length: " << r.body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << r.body;
        std::string s = os.str();
        size_t total = 0;
        while (total < s.size()) {
            ssize_t n = ::send(fd, s.data() + total, s.size() - total, 0);
            if (n <= 0) return;
            total += static_cast<size_t>(n);
        }
    }

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::map<std::string, Handler> routes_;
    std::map<std::string, RawHandler> raw_routes_;
};

// Open a TCP connection to host:port and send a POST request with the
// given body. Returns the fd if the connect+send succeeds, else -1.
// Caller is responsible for reading from / closing the fd.
//
// Used for streaming pass-through: the gateway opens a connection to
// the backend, forwards the request, then pumps response bytes back
// to the client without buffering the whole body.
inline int HttpPostStreamFD(const std::string& host, int port,
                            const std::string& path, const std::string& body,
                            int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0
        || res == nullptr) {
        ::close(fd);
        return -1;
    }
    bool ok = ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    ::freeaddrinfo(res);
    if (!ok) { ::close(fd); return -1; }

    std::ostringstream os;
    os << "POST " << path << " HTTP/1.1\r\n"
       << "Host: " << host << ":" << port << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Accept: text/event-stream\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    std::string req = os.str();
    if (::send(fd, req.data(), req.size(), 0) < 0) { ::close(fd); return -1; }
    return fd;
}

// Minimal blocking HTTP client used by the dispatcher to forward to
// backends. Connection-per-request — keep-alive is a refinement.
inline bool HttpPost(const std::string& host, int port,
                     const std::string& path, const std::string& body,
                     int timeout_ms, std::string& out) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0
        || res == nullptr) {
        ::close(fd);
        return false;
    }
    bool connected = ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    ::freeaddrinfo(res);
    if (!connected) { ::close(fd); return false; }

    std::ostringstream os;
    os << "POST " << path << " HTTP/1.1\r\n"
       << "Host: " << host << ":" << port << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    std::string req = os.str();
    if (::send(fd, req.data(), req.size(), 0) < 0) { ::close(fd); return false; }

    char tmp[4096];
    std::string raw;
    ssize_t n;
    while ((n = ::recv(fd, tmp, sizeof(tmp), 0)) > 0) raw.append(tmp, n);
    ::close(fd);
    auto h = raw.find("\r\n\r\n");
    out = h == std::string::npos ? raw : raw.substr(h + 4);
    return true;
}

}  // namespace ig
