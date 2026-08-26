#include "render/view.h"

#include <algorithm>
#include <cmath>

namespace tb::render {

ViewTransform
compute_view(uint32_t rotation, uint32_t sc_w, uint32_t sc_h, float table_w, float table_h) {
    ViewTransform v;
    v.rotation = rotation % 360u;

    const bool swapped = (v.rotation == 90u || v.rotation == 270u);
    const float lw = float(swapped ? sc_h : sc_w);
    const float lh = float(swapped ? sc_w : sc_h);

    v.ppm = std::min(lw / table_w, lh / table_h);
    v.scene_w = uint32_t(std::lround(table_w * v.ppm));
    v.scene_h = uint32_t(std::lround(table_h * v.ppm));

    // Letterbox bars: the scene rect is centered in the swapchain.
    const float drawn_w = table_w * v.ppm;
    const float drawn_h = table_h * v.ppm;
    v.bar_x = (float(sc_w) - (swapped ? drawn_h : drawn_w)) * 0.5f;
    v.bar_y = (float(sc_h) - (swapped ? drawn_w : drawn_h)) * 0.5f;
    if (v.bar_x < 0.0f) {
        v.bar_x = 0.0f;
    }
    if (v.bar_y < 0.0f) {
        v.bar_y = 0.0f;
    }
    return v;
}

void ui_to_clip(float width, float height, float out[16]) {
    const float sx = 2.0f / width;
    const float sy = -2.0f / height;
    out[0] = sx;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;

    out[4] = 0.0f;
    out[5] = sy;
    out[6] = 0.0f;
    out[7] = 0.0f;

    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 1.0f;
    out[11] = 0.0f;

    out[12] = -1.0f;
    out[13] = 1.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

void table_to_clip(float width, float height, float out[16]) {
    const float sx = 2.0f / width;
    const float sy = 2.0f / height; // +y up-table maps to NDC +y
    out[0] = sx;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;

    out[4] = 0.0f;
    out[5] = sy;
    out[6] = 0.0f;
    out[7] = 0.0f;

    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 1.0f;
    out[11] = 0.0f;

    out[12] = -1.0f;
    out[13] = -1.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

} // namespace tb::render
