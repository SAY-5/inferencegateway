// BackendPool — list of backend replicas with per-backend in-flight
// counters, health flag, and atomic stats. Routing policies operate on
// a snapshot of this structure.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ig/histogram.h"

namespace ig {

struct Backend {
    std::string id;
    std::string url;          // e.g. "http://backend-0:9000"
    std::atomic<uint32_t> inflight{0};
    std::atomic<bool> healthy{true};
    std::atomic<uint32_t> consecutive_failures{0};
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_errors{0};
    Histogram latency;       // request latency, seconds

    Backend(std::string i, std::string u) : id(std::move(i)), url(std::move(u)) {}
};

class BackendPool {
public:
    explicit BackendPool(std::vector<std::unique_ptr<Backend>> bs)
        : backends_(std::move(bs)) {}

    BackendPool(const BackendPool&) = delete;
    BackendPool& operator=(const BackendPool&) = delete;

    size_t Size() const { return backends_.size(); }

    Backend& At(size_t i) { return *backends_[i]; }
    const Backend& At(size_t i) const { return *backends_[i]; }

    // Snapshot of (idx, inflight, healthy) for the router. We don't
    // expose Backend pointers directly because the router runs from a
    // different thread and we want a cheap read-only view.
    struct View {
        size_t idx;
        uint32_t inflight;
        bool healthy;
    };

    std::vector<View> Snapshot() const {
        std::vector<View> out;
        out.reserve(backends_.size());
        for (size_t i = 0; i < backends_.size(); ++i) {
            out.push_back({
                i,
                backends_[i]->inflight.load(std::memory_order_relaxed),
                backends_[i]->healthy.load(std::memory_order_relaxed),
            });
        }
        return out;
    }

    // Health-poller helpers.
    void MarkSuccess(size_t idx) {
        backends_[idx]->consecutive_failures.store(0, std::memory_order_relaxed);
        backends_[idx]->healthy.store(true, std::memory_order_relaxed);
    }
    void MarkFailure(size_t idx, uint32_t threshold = 2) {
        uint32_t f = backends_[idx]->consecutive_failures.fetch_add(1) + 1;
        if (f >= threshold) {
            backends_[idx]->healthy.store(false, std::memory_order_relaxed);
        }
    }

    // Bookkeeping helpers used by the scheduler.
    void OnDispatch(size_t idx) {
        backends_[idx]->inflight.fetch_add(1, std::memory_order_relaxed);
        backends_[idx]->total_requests.fetch_add(1, std::memory_order_relaxed);
    }
    void OnComplete(size_t idx, double seconds, bool ok) {
        backends_[idx]->inflight.fetch_sub(1, std::memory_order_relaxed);
        backends_[idx]->latency.Observe(seconds);
        if (!ok) {
            backends_[idx]->total_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    std::vector<std::unique_ptr<Backend>> backends_;
};

}  // namespace ig
