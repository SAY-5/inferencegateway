// Unit tests for Router and Histogram. No frameworks; we use a tiny
// macro-based assert harness so the build has zero deps beyond CMake.

#include <cassert>
#include <cstdio>
#include <iostream>
#include <set>
#include <vector>

#include "ig/histogram.h"
#include "ig/router.h"

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__            \
                  << ": " << #cond << "\n";                            \
        std::exit(1);                                                  \
    }                                                                  \
} while (0)

static std::vector<ig::BackendPool::View> mkSnap(std::vector<std::pair<uint32_t, bool>> in) {
    std::vector<ig::BackendPool::View> out;
    for (size_t i = 0; i < in.size(); ++i) {
        out.push_back({i, in[i].first, in[i].second});
    }
    return out;
}

static void testRoundRobinCycles() {
    ig::Router r(ig::Policy::RoundRobin);
    auto snap = mkSnap({{0,true}, {0,true}, {0,true}});
    std::vector<int> picks;
    for (int i = 0; i < 6; ++i) picks.push_back(r.Pick(snap));
    EXPECT(picks[0] == 0);
    EXPECT(picks[1] == 1);
    EXPECT(picks[2] == 2);
    EXPECT(picks[3] == 0);
    EXPECT(picks[4] == 1);
    EXPECT(picks[5] == 2);
}

static void testRoundRobinSkipsUnhealthy() {
    ig::Router r(ig::Policy::RoundRobin);
    auto snap = mkSnap({{0,true}, {0,false}, {0,true}});
    std::set<int> seen;
    for (int i = 0; i < 10; ++i) seen.insert(r.Pick(snap));
    EXPECT(seen.count(0) > 0);
    EXPECT(seen.count(2) > 0);
    EXPECT(seen.count(1) == 0);
}

static void testLeastLoadedPicksMin() {
    ig::Router r(ig::Policy::LeastLoaded);
    auto snap = mkSnap({{5,true}, {1,true}, {3,true}});
    EXPECT(r.Pick(snap) == 1);
}

static void testLeastLoadedRespectsHealth() {
    ig::Router r(ig::Policy::LeastLoaded);
    auto snap = mkSnap({{5,true}, {0,false}, {3,true}});
    // Backend 1 has lowest load but is unhealthy; pick 2.
    EXPECT(r.Pick(snap) == 2);
}

static void testP2CFavorsLowerInflight() {
    // With 4 backends and one heavily loaded, p2c should rarely
    // pick the heavy one over many trials.
    ig::Router r(ig::Policy::PowerOfTwo);
    auto snap = mkSnap({{1,true}, {1,true}, {1,true}, {99,true}});
    int picks_heavy = 0;
    const int N = 2000;
    for (int i = 0; i < N; ++i) if (r.Pick(snap) == 3) picks_heavy++;
    // p2c should hit the heavy one ≤ ~25% of the time (random-pair
    // probability that *both* picks land on it).
    EXPECT(picks_heavy < N / 4);
}

static void testRouterReportsNoBackend() {
    ig::Router r(ig::Policy::PowerOfTwo);
    auto snap = mkSnap({{0,false}, {0,false}});
    EXPECT(r.Pick(snap) == -1);
}

static void testHistogramPercentile() {
    ig::Histogram h;
    // 50 fast (1ms) + 50 slow (100ms) observations.
    for (int i = 0; i < 50; ++i) h.Observe(0.001);
    for (int i = 0; i < 50; ++i) h.Observe(0.1);
    double p50 = h.Percentile(0.5);
    double p95 = h.Percentile(0.95);
    EXPECT(p50 <= 0.005);   // median should land in the 5ms-or-below bucket
    EXPECT(p95 >= 0.05);    // 95th lands in the 100ms-ish bucket
}

static void testHistogramBucketCounts() {
    ig::Histogram h;
    h.Observe(0.0001);  // 0.1ms — first bucket
    h.Observe(0.005);   // 5ms
    h.Observe(0.5);     // 500ms
    auto s = h.Snap();
    EXPECT(s.count == 3);
    EXPECT(s.counts[0] == 1);  // ≤0.5ms
}

int main() {
    testRoundRobinCycles();
    testRoundRobinSkipsUnhealthy();
    testLeastLoadedPicksMin();
    testLeastLoadedRespectsHealth();
    testP2CFavorsLowerInflight();
    testRouterReportsNoBackend();
    testHistogramPercentile();
    testHistogramBucketCounts();
    std::cout << "ok — all router/histogram tests passed\n";
    return 0;
}
