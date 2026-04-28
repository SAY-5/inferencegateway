// Prometheus exposition. Hand-rolled — the only dep is std::ostringstream.
#pragma once

#include <ostream>
#include <sstream>
#include <string>

#include "ig/backend_pool.h"
#include "ig/histogram.h"
#include "ig/scheduler.h"

namespace ig {

inline void writeHistogram(std::ostream& os, const std::string& name,
                           const std::string& help, const std::string& labels,
                           const Histogram& h) {
    auto snap = h.Snap();
    os << "# HELP " << name << " " << help << "\n";
    os << "# TYPE " << name << " histogram\n";
    uint64_t cum = 0;
    for (size_t i = 0; i < kDefaultBoundsSec.size(); ++i) {
        cum += snap.counts[i];
        os << name << "_bucket{" << labels;
        if (!labels.empty()) os << ",";
        os << "le=\"" << kDefaultBoundsSec[i] << "\"} " << cum << "\n";
    }
    cum += snap.counts[kDefaultBoundsSec.size()];  // +Inf
    os << name << "_bucket{" << labels;
    if (!labels.empty()) os << ",";
    os << "le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum{" << labels << "} " << (snap.sum_us / 1e6) << "\n";
    os << name << "_count{" << labels << "} " << snap.count << "\n";
}

inline std::string ExportMetrics(const BackendPool& pool, const Scheduler& sched) {
    std::ostringstream os;

    // Scheduler-level metrics.
    os << "# HELP ig_scheduler_queue_depth current depth of the dispatch queue\n";
    os << "# TYPE ig_scheduler_queue_depth gauge\n";
    os << "ig_scheduler_queue_depth " << sched.QueueDepth() << "\n";

    os << "# HELP ig_scheduler_dispatched_total requests handed to a backend\n";
    os << "# TYPE ig_scheduler_dispatched_total counter\n";
    os << "ig_scheduler_dispatched_total " << sched.Dispatched() << "\n";

    os << "# HELP ig_scheduler_dropped_total requests dropped (no healthy backend)\n";
    os << "# TYPE ig_scheduler_dropped_total counter\n";
    os << "ig_scheduler_dropped_total " << sched.Dropped() << "\n";

    writeHistogram(os, "ig_scheduler_overhead_seconds",
                   "enqueue→dispatch overhead, seconds", "",
                   sched.OverheadHistogram());

    // Per-backend metrics.
    for (size_t i = 0; i < pool.Size(); ++i) {
        const auto& b = pool.At(i);
        std::string lbl = "backend=\"" + b.id + "\"";

        os << "ig_inflight{" << lbl << "} "
           << b.inflight.load(std::memory_order_relaxed) << "\n";
        os << "ig_backend_healthy{" << lbl << "} "
           << (b.healthy.load(std::memory_order_relaxed) ? 1 : 0) << "\n";
        os << "ig_requests_total{" << lbl << "} "
           << b.total_requests.load(std::memory_order_relaxed) << "\n";
        os << "ig_errors_total{" << lbl << "} "
           << b.total_errors.load(std::memory_order_relaxed) << "\n";

        writeHistogram(os, "ig_request_duration_seconds",
                       "per-backend request latency, seconds", lbl, b.latency);
    }

    return os.str();
}

}  // namespace ig
