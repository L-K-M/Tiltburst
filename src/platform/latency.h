#pragma once

#include <atomic>
#include <cstdint>

// Latency instrumentation (05-engine-core.md §14.1): the cumulative
// input→latch histogram backing the R2.1 gate (p99.9 < 4 ms over ≥ 10,000
// press edges) plus the per-stage record ring feeding the F3 overlay.
namespace tb::input {

class LatencyHistogram {
public:
    // 128 linear bins of 62.5 µs spanning 0–8 ms, plus one overflow bin.
    static constexpr int kBinCount = 129;
    static constexpr uint64_t kBinWidthNs = 62500;
    static constexpr uint64_t kOverflowNs = 128 * kBinWidthNs; // 8 ms

    // Records one press-edge input→latch delta. Lock-free; any thread.
    void record(uint64_t delta_ns) {
        const size_t bin =
            static_cast<size_t>(delta_ns < kOverflowNs ? delta_ns / kBinWidthNs : kBinCount - 1);
        bins_[bin].fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t total() const { return total_.load(std::memory_order_relaxed); }

    // Percentile as the upper edge of the bin where the running sum first
    // reaches ceil(q · n), in nanoseconds — conservative at 62.5 µs
    // granularity (§14.1). Returns 0 for q ≤ 0 or an empty histogram.
    double percentile(double q) const;

    struct Snapshot {
        uint64_t n = 0;
        uint64_t bins[kBinCount]{};
    };

    // Consistent-enough snapshot (relaxed reads; display and gate use).
    Snapshot snapshot() const;

private:
    std::atomic<uint32_t> bins_[kBinCount]{};
    std::atomic<uint64_t> total_{0};
};

// Per-stage timestamp ring: 512 records keyed by tick (§14.1 table). The
// sim thread writes stages 1–3, the main thread completes stages 4–5 for
// the tick of the snapshot it renders. Slots are reused ring-style; a slow
// reader loses old records (drop-oldest, display-only data).
struct LatencyRecord {
    uint64_t tick = 0;
    uint64_t input_ts_ns = 0;     // stage 1: newest press edge consumed this tick
    uint64_t latch_ts_ns = 0;     // stage 2
    uint64_t publish_ts_ns = 0;   // stage 3
    uint64_t render_begin_ns = 0; // stage 4
    uint64_t present_ns = 0;      // stage 5
};

class LatencyRing {
public:
    static constexpr size_t kCapacity = 512;

    // Sim side: publish the sim-completed half of this tick's record.
    void submit_sim(const LatencyRecord& rec);

    // Main side: fill stages 4–5 on the record matching `tick` if it is
    // still resident. Returns false when the slot was overwritten.
    bool complete_main(uint64_t tick, uint64_t render_begin_ns, uint64_t present_ns);

    // Copies up to `max` most recent complete records (newest last);
    // returns how many were copied. Main-thread only.
    size_t copy_recent(LatencyRecord* out, size_t max) const;

private:
    mutable std::atomic<uint64_t> write_cursor_{0}; // total records submitted
    LatencyRecord slots_[kCapacity]{};
};

} // namespace tb::input
