#pragma once

#include "platform/display_detect.h"

#include <cstdint>

// SDL enumeration fill-in (07 §2) and the ~30 Hz backglass cadence
// logic (07 §8). The pacing state machine is GPU-free and unit-tested;
// only acquire/present touch the device.
namespace tb::platform {

// Fills DisplayInfo from SDL_GetDisplays order; false when SDL reports
// none (caller: fatal unless --headless). Declared here, defined in
// display_detect_sdl.cpp (SDL include stays out of this header).
bool enumerate_displays(std::vector<DisplayInfo>& out);

// Backglass pacing (07 §8): a deadline advanced by 33 333 333 ns per
// DRAWN frame, resynced when more than 100 ms behind (hitch), and NOT
// advanced when the acquire yields no texture (the attempt repeats
// next playfield frame). Skips are counted for the F3 overlay.
class BackglassPacer {
public:
    static constexpr uint64_t kFrameNs = 33'333'333ull; // ~30 Hz
    static constexpr uint64_t kHitchResyncNs = 100'000'000ull;

    // Call once per playfield frame with the current time. Returns
    // true when a backglass ATTEMPT is due this frame.
    bool should_attempt(uint64_t now_ns);

    // Report the attempt outcome: drawn (texture acquired + submitted)
    // or skipped (no texture / acquire error).
    void report_drawn(uint64_t now_ns);
    void report_skipped();

    uint32_t drawn_frames() const { return drawn_; }

    uint32_t skips() const { return skips_; }

private:
    // A skip does not advance next_ns_, so the deadline stays in the
    // past and should_attempt() keeps returning true until a frame is
    // drawn — the retry needs no extra flag (cycle-1 review).
    uint64_t next_ns_ = 0;
    bool primed_ = false;
    uint32_t drawn_ = 0;
    uint32_t skips_ = 0;
};

} // namespace tb::platform
