// Histogram — exponentially-bucketed latency tracker with percentile
// queries. Lock-free for the single-writer case; readers take a
// snapshot mutex briefly.
//
// Why a fixed bucket layout (not HDR or t-digest): Prometheus
// exposition expects fixed-bucket histograms, the bucket counts are
// what gets exported, and downstream tooling (PromQL
// histogram_quantile) does the rest. We never compute percentiles
// across the wire.
#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ig {

// 12 buckets in seconds, exponentially spaced. Captures 0.5ms .. 1s
// usefully; one +Inf bucket is always implicit.
inline constexpr std::array<double, 12> kDefaultBoundsSec = {
    0.0005, 0.001, 0.002, 0.005, 0.01, 0.02,
    0.05,   0.1,   0.2,   0.5,   1.0,  2.0,
};

class Histogram {
public:
    Histogram() {
        for (auto& c : counts_) c.store(0, std::memory_order_relaxed);
        sum_us_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

    // Record one observation in seconds.
    void Observe(double sec) {
        size_t i = bucketFor(sec);
        counts_[i].fetch_add(1, std::memory_order_relaxed);
        sum_us_.fetch_add(static_cast<uint64_t>(sec * 1e6), std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Atomic snapshot for exporters / percentile queries.
    struct Snapshot {
        std::array<uint64_t, kDefaultBoundsSec.size() + 1> counts{};
        uint64_t sum_us = 0;
        uint64_t count = 0;
    };

    Snapshot Snap() const {
        Snapshot s{};
        for (size_t i = 0; i < counts_.size(); ++i) {
            s.counts[i] = counts_[i].load(std::memory_order_relaxed);
        }
        s.sum_us = sum_us_.load(std::memory_order_relaxed);
        s.count = count_.load(std::memory_order_relaxed);
        return s;
    }

    // Approximate percentile (q in [0,1]) — for in-process display
    // only; Prometheus computes its own from the bucket counts.
    double Percentile(double q) const {
        Snapshot s = Snap();
        if (s.count == 0) return 0.0;
        uint64_t target = static_cast<uint64_t>(q * static_cast<double>(s.count));
        uint64_t cum = 0;
        for (size_t i = 0; i < kDefaultBoundsSec.size(); ++i) {
            cum += s.counts[i];
            if (cum >= target) return kDefaultBoundsSec[i];
        }
        return kDefaultBoundsSec.back() * 2.0; // +Inf bucket
    }

    static const std::array<double, kDefaultBoundsSec.size()>& Bounds() {
        return kDefaultBoundsSec;
    }

private:
    static size_t bucketFor(double sec) {
        for (size_t i = 0; i < kDefaultBoundsSec.size(); ++i) {
            if (sec <= kDefaultBoundsSec[i]) return i;
        }
        return kDefaultBoundsSec.size();
    }

    std::array<std::atomic<uint64_t>, kDefaultBoundsSec.size() + 1> counts_{};
    std::atomic<uint64_t> sum_us_{0};
    std::atomic<uint64_t> count_{0};
};

}  // namespace ig
