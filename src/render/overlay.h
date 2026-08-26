#pragma once

#include "render/renderer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The M1 on-screen overlay (04-milestones.md M1): fps, tick rate, frame ms,
// tick µs — rendered as stb_easy_font quads through the quad pipeline.
namespace tb::render {

struct OverlayStats {
    float fps = 0.0f;
    float tick_rate_hz = 0.0f;
    float frame_ms_last = 0.0f;
    float frame_ms_p50 = 0.0f;
    float frame_ms_p99 = 0.0f;
    float tick_us_p50 = 0.0f;
    float tick_us_p99 = 0.0f;
};

// §14.3 F3 latency-detail page inputs (05-engine-core.md §14).
struct LatencyOverlayStats {
    const char* input_source = "sdl";
    // Per-stage input→latch ms over the recent record window.
    float input_latch_ms_p50 = 0.0f;
    float input_latch_ms_p95 = 0.0f;
    float input_latch_ms_max = 0.0f;
    // Cumulative §14.1 histogram gate numbers.
    double p999_ms = 0.0;
    uint64_t press_edges = 0;
};

class Overlay {
public:
    // Formats the current stats into the text lines.
    void update(const OverlayStats& stats);

    // §14.3 F3 page: latency detail lines (input source, per-stage
    // percentiles, cumulative p99.9 gate value with sample count).
    void update_latency(const LatencyOverlayStats& stats);

    // Formatted overlay text produced by update()/update_latency().
    static constexpr size_t kLineCount = 4;

    const std::string& line(size_t i) const { return lines_[i]; }

    // Appends one QuadInstance per glyph quad to `out` at (x, y) with the
    // given color; returns the next x baseline.
    float emit_quads(float x,
                     float y,
                     const std::string& text,
                     float r,
                     float g,
                     float b,
                     float a,
                     std::vector<QuadInstance>* out,
                     uint32_t target_h) const;

private:
    OverlayStats stats_;
    std::string lines_[kLineCount];
};

} // namespace tb::render
