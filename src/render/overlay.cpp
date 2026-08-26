#include "render/overlay.h"

#include <stb_easy_font.h>

#include <algorithm>
#include <cstdio>

namespace tb::render {

namespace {

constexpr size_t kScratchBytes = 64 * 4096; // ~4k quads per print call

struct EasyFontVertex {
    float x, y, z;
    unsigned char c[4];
};

} // namespace

void Overlay::update(const OverlayStats& stats) {
    stats_ = stats;

    char buf[96];

    std::snprintf(buf, sizeof(buf), "fps %d", int(stats.fps));
    lines_[0] = buf;
    std::snprintf(buf, sizeof(buf), "tick %d Hz", int(stats.tick_rate_hz));
    lines_[1] = buf;
    std::snprintf(buf,
                  sizeof(buf),
                  "frame %.2f ms (p50 %.2f p99 %.2f)",
                  double(stats.frame_ms_last),
                  double(stats.frame_ms_p50),
                  double(stats.frame_ms_p99));
    lines_[2] = buf;
    std::snprintf(buf,
                  sizeof(buf),
                  "tick  %.2f us (p99 %.2f)",
                  double(stats.tick_us_p50),
                  double(stats.tick_us_p99));
    lines_[3] = buf;
}

void Overlay::update_latency(const LatencyOverlayStats& stats) {
    char buf[96];

    std::snprintf(buf, sizeof(buf), "input source: %s", stats.input_source);
    lines_[0] = buf;
    std::snprintf(buf,
                  sizeof(buf),
                  "input>latch p50 %.2f p95 %.2f max %.2f ms",
                  double(stats.input_latch_ms_p50),
                  double(stats.input_latch_ms_p95),
                  double(stats.input_latch_ms_max));
    lines_[1] = buf;
    std::snprintf(buf,
                  sizeof(buf),
                  "cumulative p99.9=%.2f ms (n=%llu)",
                  stats.p999_ms,
                  static_cast<unsigned long long>(stats.press_edges));
    lines_[2] = buf;
    lines_[3] = "F3 latency detail";
}

float Overlay::emit_quads(float x,
                          float y,
                          const std::string& text,
                          float r,
                          float g,
                          float b,
                          float a,
                          std::vector<QuadInstance>* out,
                          uint32_t target_h) const {
    static thread_local std::vector<unsigned char> scratch(kScratchBytes);

    unsigned char color[4] = {static_cast<unsigned char>(r * 255.f),
                              static_cast<unsigned char>(g * 255.f),
                              static_cast<unsigned char>(b * 255.f),
                              static_cast<unsigned char>(a * 255.f)};
    const int quads = stb_easy_font_print(
        x, y, const_cast<char*>(text.c_str()), color, scratch.data(), int(scratch.size()));

    const auto* verts = reinterpret_cast<const EasyFontVertex*>(scratch.data());
    for (int i = 0; i < quads; ++i) {
        const EasyFontVertex* q = verts + i * 4;
        float minx = q[0].x;
        float maxx = q[0].x;
        float miny = q[0].y;
        float maxy = q[0].y;
        for (int v = 1; v < 4; ++v) {
            minx = std::min(minx, q[v].x);
            maxx = std::max(maxx, q[v].x);
            miny = std::min(miny, q[v].y);
            maxy = std::max(maxy, q[v].y);
        }
        // Glyph quads are thin 1 px segments; pad degenerate rects so they
        // rasterize.
        const float hx = std::max((maxx - minx) * 0.5f, 0.5f);
        const float hy = std::max((maxy - miny) * 0.5f, 0.5f);
        out->push_back(
            QuadInstance{(minx + maxx) * 0.5f, (miny + maxy) * 0.5f, hx, hy, r, g, b, a});
    }
    (void)target_h;
    return x + float(stb_easy_font_width(const_cast<char*>(text.c_str())));
}

} // namespace tb::render
