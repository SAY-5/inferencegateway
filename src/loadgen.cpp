// loadgen — concurrent client that pumps /v1/completions through the
// gateway and reports throughput + latency percentiles.

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "ig/histogram.h"
#include "ig/http.h"

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 9090;
    int duration_s = 10;
    int conc = 32;
    int rate = 0;  // 0 = unlimited
    std::string body = R"({"model":"x","prompt":"hello","max_tokens":16})";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--d" && i + 1 < argc) duration_s = std::stoi(argv[++i]);
        else if (a == "--c" && i + 1 < argc) conc = std::stoi(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate = std::stoi(argv[++i]);
    }

    ig::Histogram h;
    std::atomic<uint64_t> ok{0}, err{0};
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);

    auto worker = [&](int) {
        while (std::chrono::steady_clock::now() < until) {
            std::string out;
            auto t0 = std::chrono::steady_clock::now();
            bool good = ig::HttpPost(host, port, "/v1/completions", body, 30000, out);
            auto t1 = std::chrono::steady_clock::now();
            h.Observe(std::chrono::duration<double>(t1 - t0).count());
            (good ? ok : err).fetch_add(1);
            if (rate > 0) {
                int per_thread = std::max(1, rate / std::max(1, conc));
                std::this_thread::sleep_for(std::chrono::microseconds(1000000 / per_thread));
            }
        }
    };

    std::vector<std::thread> ts;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < conc; ++i) ts.emplace_back(worker, i);
    for (auto& t : ts) t.join();
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "loadgen: ok=" << ok.load() << " err=" << err.load()
              << " elapsed=" << elapsed << "s"
              << " throughput=" << static_cast<int>((ok.load() + err.load()) / elapsed) << " rps\n";
    std::cout << "latency  p50=" << (h.Percentile(0.5) * 1000) << "ms"
              << "  p95=" << (h.Percentile(0.95) * 1000) << "ms"
              << "  p99=" << (h.Percentile(0.99) * 1000) << "ms\n";
    return 0;
}
