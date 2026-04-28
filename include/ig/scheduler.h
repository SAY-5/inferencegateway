// Scheduler — single dispatch thread that pulls from a multi-producer
// queue and hands work to a backend.
//
// The scheduler doesn't speak HTTP itself; it accepts a
// `dispatch_fn(backend_idx, request)` callback so we can test it
// end-to-end without sockets.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "ig/backend_pool.h"
#include "ig/histogram.h"
#include "ig/router.h"

namespace ig {

struct Request {
    std::string path;
    std::string body;
    std::chrono::steady_clock::time_point enqueued_at;
    // Per-request callback called when the dispatcher has picked a
    // backend and started forwarding. Tests use this to observe
    // scheduler-overhead latency.
    std::function<void(int /*backend_idx*/, const std::string& /*body*/)> on_dispatch;
};

class Scheduler {
public:
    using DispatchFn = std::function<void(int, const Request&)>;

    Scheduler(BackendPool* pool, Policy policy, DispatchFn dispatch)
        : pool_(pool), router_(policy), dispatch_(std::move(dispatch)) {}

    ~Scheduler() { Stop(); }

    void Start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread(&Scheduler::loop, this);
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void Submit(Request r) {
        r.enqueued_at = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push(std::move(r));
            queue_depth_.store(queue_.size(), std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    // Telemetry surfaces.
    uint64_t QueueDepth() const { return queue_depth_.load(std::memory_order_relaxed); }
    uint64_t Dispatched() const { return dispatched_.load(std::memory_order_relaxed); }
    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    const Histogram& OverheadHistogram() const { return overhead_; }

private:
    void loop() {
        while (running_.load()) {
            Request r;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !queue_.empty() || !running_.load(); });
                if (!running_.load() && queue_.empty()) return;
                r = std::move(queue_.front());
                queue_.pop();
                queue_depth_.store(queue_.size(), std::memory_order_relaxed);
            }
            int idx = router_.Pick(pool_->Snapshot());
            auto picked_at = std::chrono::steady_clock::now();
            double overhead_sec =
                std::chrono::duration<double>(picked_at - r.enqueued_at).count();
            overhead_.Observe(overhead_sec);
            if (idx < 0) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                if (r.on_dispatch) r.on_dispatch(-1, "no healthy backend");
                continue;
            }
            pool_->OnDispatch(static_cast<size_t>(idx));
            dispatched_.fetch_add(1, std::memory_order_relaxed);
            if (r.on_dispatch) r.on_dispatch(idx, r.body);
            dispatch_(idx, r);
        }
    }

    BackendPool* pool_;
    Router router_;
    DispatchFn dispatch_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Request> queue_;
    std::atomic<uint64_t> queue_depth_{0};
    std::atomic<uint64_t> dispatched_{0};
    std::atomic<uint64_t> dropped_{0};
    Histogram overhead_;
};

}  // namespace ig
