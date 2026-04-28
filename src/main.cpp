// inferenceg — gateway entry point.
//
// Wires HttpServer → Scheduler → BackendPool → HttpPost dispatcher.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ig/backend_pool.h"
#include "ig/http.h"
#include "ig/metrics.h"
#include "ig/router.h"
#include "ig/scheduler.h"

namespace {

ig::Backend* parseBackendURL(const std::string& url, std::string& host, int& port,
                             std::string& path) {
    // http://host:port/path
    auto p = url.find("://");
    std::string rest = p == std::string::npos ? url : url.substr(p + 3);
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostport.find(':');
    host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    port = colon == std::string::npos ? 80 : std::stoi(hostport.substr(colon + 1));
    return nullptr;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string listen = "0.0.0.0";
    int port = 9090;
    std::string backends_csv = "http://127.0.0.1:9001,http://127.0.0.1:9002,http://127.0.0.1:9003,http://127.0.0.1:9004";
    std::string policy_str = "p2c";
    int workers = 32;
    int dispatch_timeout_ms = 30000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--port"     && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--listen"   && i + 1 < argc) listen = argv[++i];
        else if (a == "--backends" && i + 1 < argc) backends_csv = argv[++i];
        else if (a == "--policy"   && i + 1 < argc) policy_str = argv[++i];
        else if (a == "--workers"  && i + 1 < argc) workers = std::stoi(argv[++i]);
        else if (a == "--timeout"  && i + 1 < argc) dispatch_timeout_ms = std::stoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "inferenceg — LLM gateway\n"
                      << "  --port <n>          (default 9090)\n"
                      << "  --listen <addr>     (default 0.0.0.0)\n"
                      << "  --backends <csv>    comma-separated http://host:port URLs\n"
                      << "  --policy <name>     round_robin|p2c|least_loaded|random (default p2c)\n"
                      << "  --workers <n>       HTTP server thread pool (default 32)\n"
                      << "  --timeout <ms>      backend dispatch timeout (default 30000)\n";
            return 0;
        }
    }

    auto urls = split(backends_csv, ',');
    std::vector<std::unique_ptr<ig::Backend>> bs;
    for (size_t i = 0; i < urls.size(); ++i) {
        bs.push_back(std::make_unique<ig::Backend>("b" + std::to_string(i), urls[i]));
    }
    ig::BackendPool pool(std::move(bs));
    ig::Policy policy = ig::Router::Parse(policy_str);

    // The dispatch lambda — runs on the scheduler's worker thread.
    // For real throughput we'd hand this to a thread pool; for the
    // demo, blocking here is fine because the scheduler thread is
    // explicitly the choke point we're measuring.
    ig::Scheduler scheduler(&pool, policy, [&](int idx, const ig::Request& req) {
        std::string host, b_path; int b_port;
        const auto& backend = pool.At(static_cast<size_t>(idx));
        parseBackendURL(backend.url, host, b_port, b_path);
        std::string fwd_path = b_path == "/" ? req.path : b_path + req.path;
        std::string out;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = ig::HttpPost(host, b_port, fwd_path, req.body, dispatch_timeout_ms, out);
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        pool.OnComplete(static_cast<size_t>(idx), sec, ok);
    });
    scheduler.Start();

    // Health poller — every 5s, mark a backend unhealthy after 2
    // consecutive failures.
    std::atomic<bool> hp_running{true};
    std::thread hp([&] {
        while (hp_running.load()) {
            for (size_t i = 0; i < pool.Size(); ++i) {
                std::string host, b_path; int b_port;
                parseBackendURL(pool.At(i).url, host, b_port, b_path);
                std::string out;
                bool ok = ig::HttpPost(host, b_port, "/v1/models", "{}", 1000, out);
                if (ok) pool.MarkSuccess(i);
                else    pool.MarkFailure(i);
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    ig::HttpServer srv;
    srv.Route("GET", "/healthz", [](const ig::HttpRequest&) {
        return ig::HttpResponse{200, "application/json", "{\"ok\":true}"};
    });
    srv.Route("GET", "/metrics", [&](const ig::HttpRequest&) {
        return ig::HttpResponse{200, "text/plain; version=0.0.4", ig::ExportMetrics(pool, scheduler)};
    });
    srv.Route("GET", "/v1/cluster", [&](const ig::HttpRequest&) {
        std::ostringstream os;
        os << "{\"backends\":[";
        for (size_t i = 0; i < pool.Size(); ++i) {
            const auto& b = pool.At(i);
            os << (i ? "," : "")
               << "{\"id\":\"" << b.id << "\",\"url\":\"" << b.url << "\","
               << "\"healthy\":" << (b.healthy.load() ? "true" : "false") << ","
               << "\"inflight\":" << b.inflight.load() << ","
               << "\"requests\":" << b.total_requests.load() << ","
               << "\"errors\":" << b.total_errors.load() << "}";
        }
        os << "],\"policy\":\"" << ig::Router::Name(policy) << "\","
           << "\"queue_depth\":" << scheduler.QueueDepth() << ","
           << "\"dispatched\":" << scheduler.Dispatched() << ","
           << "\"dropped\":" << scheduler.Dropped() << "}";
        return ig::HttpResponse{200, "application/json", os.str()};
    });
    auto forward = [&](const ig::HttpRequest& req) {
        ig::Request r;
        r.path = req.path;
        r.body = req.body;
        // We block the HTTP worker until the scheduler dispatches. Real
        // production uses an async response loop; this keeps the demo
        // simple. Shared state lives in `slot` below; the dispatcher's
        // on_dispatch hook writes the chosen backend index, then we do
        // the actual HTTP forward inline so we can return the response
        // body to the caller.
        struct Slot {
            std::mutex m;
            std::condition_variable c;
            bool ready = false;
            std::string out;
            int idx = -1;
        };
        auto slot = std::make_shared<Slot>();
        // Replace the dispatcher's outbound: we re-dispatch from here
        // instead of relying on the scheduler's stored callback. The
        // scheduler still handles routing + bookkeeping; we wait for
        // its on_dispatch then do the HTTP call inline so we can
        // forward the response.
        r.on_dispatch = [slot](int idx, const std::string&) {
            std::lock_guard<std::mutex> lk(slot->m);
            slot->idx = idx;
            slot->ready = true;
            slot->c.notify_one();
        };
        scheduler.Submit(std::move(r));

        std::unique_lock<std::mutex> lk(slot->m);
        slot->c.wait_for(lk, std::chrono::seconds(30), [&] { return slot->ready; });
        if (slot->idx < 0) {
            return ig::HttpResponse{503, "application/json",
                "{\"ok\":false,\"err\":\"no healthy backend\"}"};
        }
        std::string host, b_path; int b_port;
        const auto& b = pool.At(static_cast<size_t>(slot->idx));
        parseBackendURL(b.url, host, b_port, b_path);
        std::string out;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = ig::HttpPost(host, b_port, req.path, req.body, dispatch_timeout_ms, out);
        auto t1 = std::chrono::steady_clock::now();
        pool.OnComplete(static_cast<size_t>(slot->idx),
                        std::chrono::duration<double>(t1 - t0).count(), ok);
        if (!ok) {
            return ig::HttpResponse{502, "application/json",
                "{\"ok\":false,\"err\":\"backend call failed\"}"};
        }
        return ig::HttpResponse{200, "application/json", out};
    };
    srv.Route("POST", "/v1/completions", forward);
    srv.Route("POST", "/v1/chat/completions", forward);

    std::cout << "inferenceg listening " << listen << ":" << port
              << " · " << pool.Size() << " backend(s)"
              << " · policy=" << ig::Router::Name(policy) << "\n";
    if (!srv.Listen(listen, port, workers)) {
        std::cerr << "failed to bind " << listen << ":" << port << "\n";
        return 1;
    }
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
    hp_running.store(false);
    if (hp.joinable()) hp.join();
    return 0;
}
