#include "platform/backglass_pacer.h"

namespace tb::platform {

bool BackglassPacer::should_attempt(uint64_t now_ns) {
    if (!primed_) {
        next_ns_ = now_ns;
        primed_ = true;
    }
    // Resync after a hitch: more than 100 ms BEHIND means wall time
    // jumped (debugger, GPU stall) — snap the deadline to now. The
    // ahead-of-deadline case must not underflow the unsigned delta.
    if (now_ns > next_ns_ && now_ns - next_ns_ > kHitchResyncNs) {
        next_ns_ = now_ns;
    }
    return now_ns >= next_ns_;
}

void BackglassPacer::report_drawn(uint64_t now_ns) {
    ++drawn_;
    // Advance from the DEADLINE, not from now: a frame drawn 5 ms late
    // does not slide the whole cadence late.
    next_ns_ += kFrameNs;
    if (now_ns > next_ns_ && now_ns - next_ns_ > kHitchResyncNs) {
        next_ns_ = now_ns; // still hopelessly behind: resync
    }
}

void BackglassPacer::report_skipped() {
    ++skips_;
    // Deadline NOT advanced (07 §8): the attempt repeats next frame.
}

} // namespace tb::platform
