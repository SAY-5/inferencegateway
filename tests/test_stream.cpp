// Streaming pass-through smoke test.
//
// Spins up:
//   - one fakebackend on a free port (its raw `/v1/.../stream` route
//     emits 5 SSE frames), and
//   - the gateway on another port pointing at that one backend,
// then opens a raw TCP connection to the gateway, POSTs a chat
// completion request to the streaming route, and asserts the response
// is text/event-stream with `data: [DONE]` in the body.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ig/backend_pool.h"
#include "ig/http.h"
#include "ig/router.h"
#include "ig/scheduler.h"

#define EXPECT(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #c << "\n"; std::exit(1); } } while (0)

static int freePort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t sz = sizeof(a); ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &sz);
    int p = ntohs(a.sin_port);
    ::close(s);
    return p;
}

static std::string sendAndCollect(int port, const std::string& path, const std::string& body) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return ""; }
    std::ostringstream rq;
    rq << "POST " << path << " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       << "Content-Type: application/json\r\nContent-Length: " << body.size()
       << "\r\nConnection: close\r\n\r\n" << body;
    auto s = rq.str();
    ::send(fd, s.data(), s.size(), 0);
    std::string out;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, n);
    }
    ::close(fd);
    return out;
}

int main() {
    // Build a backend pool with a single fake backend on a free port,
    // then run the same forward+route wiring main.cpp uses (minus the
    // health poller, which would otherwise hit /v1/models and fail).
    int backendPort = freePort();

    // Spin up a fakebackend-equivalent inline (no subprocess needed):
    // a raw SSE responder on /v1/chat/completions/stream.
    ig::HttpServer be;
    be.RouteRaw("POST", "/v1/chat/completions/stream", [](const ig::RawContext& ctx) -> bool {
        const char* hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
        ::send(ctx.fd, hdr, std::strlen(hdr), 0);
        const char* frames[] = {
            "data: {\"delta\":\"hel\"}\n\n",
            "data: {\"delta\":\"lo\"}\n\n",
            "data: [DONE]\n\n",
        };
        for (auto* f : frames) ::send(ctx.fd, f, std::strlen(f), 0);
        return true;
    });
    EXPECT(be.Listen("127.0.0.1", backendPort, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Build a gateway pointing at that backend.
    std::vector<std::unique_ptr<ig::Backend>> bs;
    bs.push_back(std::make_unique<ig::Backend>("b0", "http://127.0.0.1:" + std::to_string(backendPort)));
    ig::BackendPool pool(std::move(bs));
    pool.MarkSuccess(0);  // skip the health poller; declare healthy.

    int gwPort = freePort();
    ig::Scheduler scheduler(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request&) {
            // The streaming forward path performs its own HTTP call;
            // the scheduler's dispatch lambda is only used by the
            // non-streaming path. Mark complete with zero work so we
            // don't double-count.
            pool.OnComplete(static_cast<size_t>(idx), 0.0, true);
        });
    scheduler.Start();

    ig::HttpServer gw;
    gw.RouteRaw("POST", "/v1/chat/completions/stream", [&](const ig::RawContext& ctx) -> bool {
        struct Slot { std::mutex m; std::condition_variable c; bool ready = false; int idx = -1; };
        auto slot = std::make_shared<Slot>();
        ig::Request r;
        r.path = ctx.req.path;
        r.body = ctx.req.body;
        r.on_dispatch = [slot](int idx, const std::string&) {
            std::lock_guard<std::mutex> lk(slot->m);
            slot->idx = idx; slot->ready = true; slot->c.notify_one();
        };
        scheduler.Submit(std::move(r));
        std::unique_lock<std::mutex> lk(slot->m);
        slot->c.wait_for(lk, std::chrono::seconds(5), [&] { return slot->ready; });
        lk.unlock();
        EXPECT(slot->idx == 0);

        int bfd = ig::HttpPostStreamFD("127.0.0.1", backendPort, ctx.req.path, ctx.req.body, 5000);
        EXPECT(bfd >= 0);

        const char* hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
        ::send(ctx.fd, hdr, std::strlen(hdr), 0);
        char buf[1024];
        std::string preamble; bool past = false;
        for (;;) {
            ssize_t n = ::recv(bfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!past) {
                preamble.append(buf, n);
                auto h = preamble.find("\r\n\r\n");
                if (h == std::string::npos) continue;
                past = true;
                auto rem = preamble.substr(h + 4);
                if (!rem.empty()) ::send(ctx.fd, rem.data(), rem.size(), 0);
                continue;
            }
            ::send(ctx.fd, buf, n, 0);
        }
        ::close(bfd);
        return true;
    });
    EXPECT(gw.Listen("127.0.0.1", gwPort, 4));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = sendAndCollect(gwPort, "/v1/chat/completions/stream",
                               R"({"model":"x","messages":[]})");
    EXPECT(resp.find("HTTP/1.1 200") != std::string::npos);
    EXPECT(resp.find("text/event-stream") != std::string::npos);
    EXPECT(resp.find("data: [DONE]") != std::string::npos);
    EXPECT(resp.find("\"delta\":\"hel\"") != std::string::npos);
    EXPECT(resp.find("\"delta\":\"lo\"") != std::string::npos);

    gw.Stop();
    be.Stop();
    scheduler.Stop();
    std::cout << "ok — streaming pass-through end-to-end\n";
    return 0;
}
