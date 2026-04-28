// fakebackend — a minimal LLM-replica simulator.
// Echoes a deterministic 200-token response after a configurable
// delay. Used by loadgen and CI to exercise the gateway under load.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "ig/http.h"

int main(int argc, char** argv) {
    int port = 9001;
    int min_ms = 8, max_ms = 24;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--min-ms" && i + 1 < argc) min_ms = std::stoi(argv[++i]);
        else if (a == "--max-ms" && i + 1 < argc) max_ms = std::stoi(argv[++i]);
    }

    std::mt19937_64 rng(static_cast<uint64_t>(port) * 0xa5a5a5a5);
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    std::atomic<uint64_t> served{0};

    ig::HttpServer srv;
    srv.Route("POST", "/v1/completions", [&](const ig::HttpRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));
        served.fetch_add(1);
        return ig::HttpResponse{200, "application/json",
            R"({"choices":[{"text":"this is a fake completion","finish_reason":"stop"}]})"};
    });
    srv.Route("POST", "/v1/chat/completions", [&](const ig::HttpRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));
        served.fetch_add(1);
        return ig::HttpResponse{200, "application/json",
            R"({"choices":[{"message":{"role":"assistant","content":"fake reply"}}]})"};
    });
    srv.Route("POST", "/v1/models", [&](const ig::HttpRequest&) {
        return ig::HttpResponse{200, "application/json", R"({"data":[{"id":"fake-1"}]})"};
    });
    srv.Route("GET", "/healthz", [&](const ig::HttpRequest&) {
        std::ostringstream os;
        os << "{\"ok\":true,\"served\":" << served.load() << "}";
        return ig::HttpResponse{200, "application/json", os.str()};
    });

    std::cout << "fakebackend listening :" << port
              << " · delay=" << min_ms << "-" << max_ms << "ms\n";
    if (!srv.Listen("0.0.0.0", port, 16)) return 1;
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
