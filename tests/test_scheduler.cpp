// End-to-end scheduler test — submit → dispatch → backend bookkeeping
// → metrics. No real I/O.

#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>

#include "ig/backend_pool.h"
#include "ig/metrics.h"
#include "ig/scheduler.h"

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__            \
                  << ": " << #cond << "\n";                            \
        std::exit(1);                                                  \
    }                                                                  \
} while (0)

static ig::BackendPool makePool(size_t n) {
    std::vector<std::unique_ptr<ig::Backend>> bs;
    for (size_t i = 0; i < n; ++i)
        bs.push_back(std::make_unique<ig::Backend>(
            "b" + std::to_string(i), "http://x"));
    return ig::BackendPool(std::move(bs));
}

static void testSchedulerDispatchesAllRequests() {
    auto pool = makePool(3);
    std::atomic<int> dispatches{0};
    ig::Scheduler s(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request& r) {
            (void)r;
            EXPECT(idx >= 0 && idx < 3);
            // Simulate a tiny backend call.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pool.OnComplete(static_cast<size_t>(idx), 0.002, true);
            dispatches.fetch_add(1);
        });
    s.Start();
    for (int i = 0; i < 30; ++i) s.Submit({"/v1/completions", "{}", "", {}, nullptr});
    // Wait for drain.
    for (int i = 0; i < 200 && dispatches.load() < 30; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    s.Stop();
    EXPECT(dispatches.load() == 30);
    EXPECT(s.QueueDepth() == 0);
    EXPECT(s.Dispatched() == 30);
}

static void testSchedulerDropsWhenAllUnhealthy() {
    auto pool = makePool(2);
    pool.At(0).healthy.store(false);
    pool.At(1).healthy.store(false);
    std::atomic<int> calls{0};
    ig::Scheduler s(&pool, ig::Policy::PowerOfTwo,
        [&](int, const ig::Request&) { calls.fetch_add(1); });
    s.Start();
    for (int i = 0; i < 5; ++i) s.Submit({"/v1/completions", "{}", "", {}, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.Stop();
    EXPECT(calls.load() == 0);
    EXPECT(s.Dropped() == 5);
}

static void testSchedulerOverheadMetric() {
    auto pool = makePool(2);
    ig::Scheduler s(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request&) { pool.OnComplete(static_cast<size_t>(idx), 0.0, true); });
    s.Start();
    for (int i = 0; i < 200; ++i) s.Submit({"/v1/completions", "{}", "", {}, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    s.Stop();
    auto p99 = s.OverheadHistogram().Percentile(0.99);
    // At idle scheduler, overhead should be well under 50ms.
    EXPECT(p99 <= 0.05);
}

static void testMetricsExpositionShapeSane() {
    auto pool = makePool(2);
    ig::Scheduler s(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request&) { pool.OnComplete(static_cast<size_t>(idx), 0.001, true); });
    s.Start();
    for (int i = 0; i < 5; ++i) s.Submit({"/v1/completions", "{}", "", {}, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s.Stop();
    std::string out = ig::ExportMetrics(pool, s);
    // Spot-check: must contain a few key metric names.
    EXPECT(out.find("ig_scheduler_dispatched_total") != std::string::npos);
    EXPECT(out.find("ig_request_duration_seconds_bucket") != std::string::npos);
    EXPECT(out.find("ig_backend_healthy") != std::string::npos);
}

int main() {
    testSchedulerDispatchesAllRequests();
    testSchedulerDropsWhenAllUnhealthy();
    testSchedulerOverheadMetric();
    testMetricsExpositionShapeSane();
    std::cout << "ok — all scheduler/metrics tests passed\n";
    return 0;
}
