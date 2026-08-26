#include "platform/latency.h"

#include <algorithm>
#include <cmath>

namespace tb::input {

double LatencyHistogram::percentile(double q) const {
    const Snapshot snap = snapshot();
    if (snap.n == 0 || q <= 0.0) {
        return 0.0;
    }
    const double target = std::ceil(q * double(snap.n));
    uint64_t running = 0;
    for (int i = 0; i < kBinCount; ++i) {
        running += snap.bins[i];
        if (double(running) >= target) {
            if (i == kBinCount - 1) {
                return double(kOverflowNs); // overflow bin reports 8 ms
            }
            // Conservative: report the bin's upper edge.
            return double(uint64_t(i + 1) * kBinWidthNs);
        }
    }
    return double(kOverflowNs);
}

LatencyHistogram::Snapshot LatencyHistogram::snapshot() const {
    Snapshot snap;
    snap.n = total_.load(std::memory_order_relaxed);
    for (int i = 0; i < kBinCount; ++i) {
        snap.bins[i] = bins_[i].load(std::memory_order_relaxed);
    }
    return snap;
}

void LatencyRing::submit_sim(const LatencyRecord& rec) {
    const uint64_t cursor = write_cursor_.fetch_add(1, std::memory_order_relaxed);
    slots_[cursor % kCapacity] = rec;
}

bool LatencyRing::complete_main(uint64_t tick, uint64_t render_begin_ns, uint64_t present_ns) {
    for (size_t i = 0; i < kCapacity; ++i) {
        LatencyRecord& slot = slots_[i];
        if (slot.tick == tick && slot.publish_ts_ns != 0 && slot.present_ns == 0) {
            slot.render_begin_ns = render_begin_ns;
            slot.present_ns = present_ns;
            return true;
        }
    }
    return false;
}

size_t LatencyRing::copy_recent(LatencyRecord* out, size_t max) const {
    const uint64_t cursor = write_cursor_.load(std::memory_order_relaxed);
    const size_t available = static_cast<size_t>(std::min<uint64_t>(cursor, kCapacity));
    const size_t count = std::min(available, max);
    for (size_t i = 0; i < count; ++i) {
        out[i] = slots_[(cursor - count + i) % kCapacity];
    }
    return count;
}

} // namespace tb::input
