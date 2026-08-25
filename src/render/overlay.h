#pragma once

#include "render/renderer.h"

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

class Overlay {
public:
    // Rebuilds the text layout for the current stats into a quad list in
    // UI pixel space (top-left origin). Cheap: called once per frame.
    void update(const OverlayStats& stats);

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
    std::string lines_[6];
};

} // namespace tb::render
