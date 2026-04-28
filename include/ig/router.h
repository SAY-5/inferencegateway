// Router — pure-functional policies that select a backend index from
// a BackendPool::Snapshot.
//
// Keeping these stateless lets us unit-test them table-driven against
// crafted snapshots. The scheduler holds the only mutable state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "ig/backend_pool.h"

namespace ig {

enum class Policy {
    RoundRobin,
    PowerOfTwo,    // p2c
    LeastLoaded,
    Random,
};

class Router {
public:
    explicit Router(Policy policy, uint64_t rng_seed = 0xc1c2c3c4)
        : policy_(policy), rng_(rng_seed) {}

    // Returns the chosen index, or -1 if no backend is healthy.
    int Pick(const std::vector<BackendPool::View>& snap) {
        if (snap.empty()) return -1;
        switch (policy_) {
        case Policy::RoundRobin: return pickRR(snap);
        case Policy::PowerOfTwo: return pickP2C(snap);
        case Policy::LeastLoaded: return pickLeast(snap);
        case Policy::Random: return pickRandom(snap);
        }
        return -1;
    }

    static const char* Name(Policy p) {
        switch (p) {
        case Policy::RoundRobin: return "round_robin";
        case Policy::PowerOfTwo: return "p2c";
        case Policy::LeastLoaded: return "least_loaded";
        case Policy::Random: return "random";
        }
        return "?";
    }

    static Policy Parse(const std::string& s) {
        if (s == "round_robin") return Policy::RoundRobin;
        if (s == "p2c" || s == "power_of_two") return Policy::PowerOfTwo;
        if (s == "least_loaded") return Policy::LeastLoaded;
        if (s == "random") return Policy::Random;
        return Policy::PowerOfTwo;
    }

private:
    int pickRR(const std::vector<BackendPool::View>& snap) {
        size_t n = snap.size();
        for (size_t step = 0; step < n; ++step) {
            size_t idx = (rr_cursor_ + step) % n;
            if (snap[idx].healthy) {
                rr_cursor_ = (idx + 1) % n;
                return static_cast<int>(snap[idx].idx);
            }
        }
        return -1;
    }

    int pickP2C(const std::vector<BackendPool::View>& snap) {
        std::vector<size_t> healthy_idx;
        healthy_idx.reserve(snap.size());
        for (size_t i = 0; i < snap.size(); ++i) {
            if (snap[i].healthy) healthy_idx.push_back(i);
        }
        if (healthy_idx.empty()) return -1;
        if (healthy_idx.size() == 1) return static_cast<int>(snap[healthy_idx[0]].idx);
        std::uniform_int_distribution<size_t> dist(0, healthy_idx.size() - 1);
        size_t a = healthy_idx[dist(rng_)];
        size_t b = healthy_idx[dist(rng_)];
        // If we picked the same one twice, just use it.
        const auto& va = snap[a];
        const auto& vb = snap[b];
        return static_cast<int>(va.inflight <= vb.inflight ? va.idx : vb.idx);
    }

    int pickLeast(const std::vector<BackendPool::View>& snap) {
        int best = -1;
        uint32_t best_load = UINT32_MAX;
        for (const auto& v : snap) {
            if (!v.healthy) continue;
            if (v.inflight < best_load) {
                best_load = v.inflight;
                best = static_cast<int>(v.idx);
            }
        }
        return best;
    }

    int pickRandom(const std::vector<BackendPool::View>& snap) {
        std::vector<size_t> healthy_idx;
        for (size_t i = 0; i < snap.size(); ++i) {
            if (snap[i].healthy) healthy_idx.push_back(i);
        }
        if (healthy_idx.empty()) return -1;
        std::uniform_int_distribution<size_t> dist(0, healthy_idx.size() - 1);
        return static_cast<int>(snap[healthy_idx[dist(rng_)]].idx);
    }

    Policy policy_;
    std::mt19937_64 rng_;
    size_t rr_cursor_ = 0;
};

}  // namespace ig
