#pragma once

#include "sim/types.h"

#include <atomic>
#include <cstdint>

// SimSnapshot and the triple buffer (05-engine-core.md §7).
//
// Grows milestone by milestone; at M2 it carries tick, sim time, and ball
// positions (§7.1 subset). POD, fixed size, no pointers, safe to memcpy.
namespace tb {

using tb::sim::kMaxBalls;

struct BallSnap {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float z = 0.0f;
    float omega = 0.0f;
    uint8_t layer = 0;
    uint8_t flags = 0; // bit0 active
    uint16_t _pad = 0;
};

struct SimSnapshot {
    uint64_t tick = 0;
    double sim_time_s = 0.0; // tick * 0.001 exactly
    uint32_t ball_count = 0;
    uint32_t _pad = 0;
    BallSnap balls[kMaxBalls]{};

    // Tilt-danger telemetry (08 §7.2/§7.3), published for overlays.
    float tilt_px = 0.0f, tilt_py = 0.0f; // bob position (m)
    float tilt_vx = 0.0f, tilt_vy = 0.0f; // bob velocity (m/s)
    float tilt_abuse = 0.0f;              // abuse accumulator (m/s)
    uint16_t tilt_crossings = 0;          // per-ball crossing count
    uint8_t tilt_armed = 0x7;             // warn|hard|abuse arm bits
};

// Triple buffer (§7.2, binding). Single writer (sim), single reader
// (main). Three slots; at every moment the indices {0,1,2} are partitioned
// between latest_, the writer slot, and the reader slot. `fresh`
// distinguishes "new since last read".
//
// Memory-ordering rationale (load-bearing): the writer's exchange is a
// release so all slot writes are visible to a reader that acquires the
// same value; both exchanges are acq_rel because each side also takes
// ownership of the slot the other side surrendered and must not reorder
// its slot access before the exchange. Never "optimize" these to relaxed.
// No CAS loops, no waiting: one unconditional atomic exchange per side.
template <typename T>
class TripleBuffer {
public:
    // ---- writer side (sim thread ONLY) ----
    T& write_slot() { return slots_[write_idx_]; } // fill, then publish()

    void publish() {
        uint32_t prev = latest_.exchange(write_idx_ | kFresh, std::memory_order_acq_rel);
        write_idx_ = prev & kIndexMask; // recycle whichever slot we displaced
    }

    // ---- reader side (main thread ONLY) ----
    // Returned reference is valid until the NEXT read() call.
    const T& read() {
        if (latest_.load(std::memory_order_relaxed) & kFresh) {
            uint32_t prev = latest_.exchange(read_idx_, std::memory_order_acq_rel);
            read_idx_ = prev & kIndexMask;
        }
        return slots_[read_idx_];
    }

private:
    static constexpr uint32_t kIndexMask = 0x3u;
    static constexpr uint32_t kFresh = 0x4u;
    mutable T slots_[3]{};
    mutable std::atomic<uint32_t> latest_{0}; // slot 0, not fresh
    mutable uint32_t write_idx_ = 1;          // writer-owned slot
    mutable uint32_t read_idx_ = 2;           // reader-owned slot
};

// 04-milestones.md M1 interface over the §7.2 algorithm.
class SnapshotBuffer {
public:
    void publish(const SimSnapshot& snap) { // sim thread
        buffer_.write_slot() = snap;
        buffer_.publish();
    }

    SimSnapshot acquire_latest() const { // main thread ONLY (single reader)
        return buffer_.read();
    }

private:
    mutable TripleBuffer<SimSnapshot> buffer_;
};

} // namespace tb
