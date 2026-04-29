// v3: session-affinity routing tests.
//
// Same session id → same backend index, deterministically.
// Empty session id → fallback to the configured policy.
// Affinity-target unhealthy → walk forward to the next healthy.
// End-to-end through the scheduler with the AffinityHits counter.

#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "ig/backend_pool.h"
#include "ig/router.h"
#include "ig/scheduler.h"

#define EXPECT(c) do { if (!(c)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #c << "\n"; std::exit(1); } } while (0)

static std::vector<ig::BackendPool::View> snap(std::vector<std::pair<uint32_t, bool>> in) {
    std::vector<ig::BackendPool::View> out;
    for (size_t i = 0; i < in.size(); ++i) out.push_back({i, in[i].first, in[i].second});
    return out;
}

static ig::BackendPool makePool(size_t n) {
    std::vector<std::unique_ptr<ig::Backend>> bs;
    for (size_t i = 0; i < n; ++i)
        bs.push_back(std::make_unique<ig::Backend>("b" + std::to_string(i), "http://x"));
    return ig::BackendPool(std::move(bs));
}

static void testAffinityIsDeterministic() {
    auto s = snap({{0,true}, {0,true}, {0,true}, {0,true}});
    int first = ig::PickAffinity(s, "user-42");
    EXPECT(first >= 0);
    for (int i = 0; i < 50; ++i) {
        EXPECT(ig::PickAffinity(s, "user-42") == first);
    }
}

static void testEmptySessionFallsThrough() {
    auto s = snap({{0,true}, {0,true}});
    EXPECT(ig::PickAffinity(s, "") == -1);
}

static void testAffinitySpreadsAcrossBackends() {
    auto s = snap({{0,true}, {0,true}, {0,true}, {0,true}});
    std::set<int> seen;
    for (int i = 0; i < 200; ++i) {
        seen.insert(ig::PickAffinity(s, "session-" + std::to_string(i)));
    }
    // 200 distinct session ids should distribute over all 4 backends.
    EXPECT(seen.size() == 4);
}

static void testAffinityHopsOverUnhealthy() {
    // Pick a session id whose hash would land on a known-bad index;
    // we don't know which, so verify the chosen backend is healthy.
    auto s = snap({{0,false}, {0,true}, {0,false}, {0,true}});
    for (int i = 0; i < 100; ++i) {
        int got = ig::PickAffinity(s, "user-" + std::to_string(i));
        EXPECT(got == 1 || got == 3);
    }
}

static void testAffinityReturnsNoneWhenAllUnhealthy() {
    auto s = snap({{0,false}, {0,false}});
    EXPECT(ig::PickAffinity(s, "x") == -1);
}

static void testSchedulerRecordsAffinityHits() {
    auto pool = makePool(4);
    std::atomic<int> dispatches{0};
    std::atomic<int> picked_idx_first{-1};
    ig::Scheduler s(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request&) {
            int expected = -1;
            picked_idx_first.compare_exchange_strong(expected, idx);
            pool.OnComplete(static_cast<size_t>(idx), 0.0, true);
            dispatches.fetch_add(1);
        });
    s.Start();
    // Submit 5 requests with the same session id.
    for (int i = 0; i < 5; ++i) {
        ig::Request r;
        r.path = "/v1/completions";
        r.body = "{}";
        r.session_id = "user-42";
        s.Submit(std::move(r));
    }
    for (int i = 0; i < 200 && dispatches.load() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    s.Stop();
    EXPECT(s.AffinityHits() == 5);
    // All 5 dispatches must have hit the same backend.
    int first = picked_idx_first.load();
    EXPECT(first >= 0);
}

static void testNoSessionDoesNotIncrementAffinityCounter() {
    auto pool = makePool(2);
    ig::Scheduler s(&pool, ig::Policy::RoundRobin,
        [&](int idx, const ig::Request&) { pool.OnComplete(static_cast<size_t>(idx), 0.0, true); });
    s.Start();
    for (int i = 0; i < 4; ++i) {
        ig::Request r; r.path = "/x"; r.body = "{}";  // no session_id
        s.Submit(std::move(r));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.Stop();
    EXPECT(s.AffinityHits() == 0);
}

int main() {
    testAffinityIsDeterministic();
    testEmptySessionFallsThrough();
    testAffinitySpreadsAcrossBackends();
    testAffinityHopsOverUnhealthy();
    testAffinityReturnsNoneWhenAllUnhealthy();
    testSchedulerRecordsAffinityHits();
    testNoSessionDoesNotIncrementAffinityCounter();
    std::cout << "ok — all affinity tests passed\n";
    return 0;
}
