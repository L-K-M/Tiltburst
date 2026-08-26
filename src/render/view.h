#pragma once

#include "sim/math.h"

#include <cstdint>

// Table→scene→swapchain transforms (06-rendering.md §6.2–§6.4). Pure
// math, unit-testable without a GPU.
namespace tb::render {

constexpr float kReferenceTableW = 0.52f;
constexpr float kReferenceTableH = 1.04f;

struct ViewTransform {
    uint32_t rotation = 0; // 0 | 90 | 180 | 270 (§6.4 corner table)
    float ppm = 0.0f;      // pixels per meter in logical space
    uint32_t scene_w = 0;  // scene target size (portrait, unrotated)
    uint32_t scene_h = 0;
    float bar_x = 0.0f; // swapchain letterbox bars (pixels)
    float bar_y = 0.0f;
};

// §6.2: logical size = swapchain with 90/270 axes swapped; ppm fits the
// table into the LOGICAL rect; the scene target covers exactly the play
// area.
ViewTransform
compute_view(uint32_t rotation, uint32_t sc_w, uint32_t sc_h, float table_w, float table_h);

// Logical pixel → clip-space NDC with the UI y flip (§6.4).
void ui_to_clip(float width, float height, float out[16]);

// Table meters → scene-pixel NDC (scene target covers exactly the play
// area: x_ndc = x/w*2-1, y_ndc = y/h*2-1).
void table_to_clip(float width, float height, float out[16]);

// §6.4 corner transform on NDC coordinates for the composite quad.
// Returns the rotated pair per the rot ∈ {0,90,180,270} table.
inline void rotate_ndc(uint32_t rotation, float x, float y, float& ox, float& oy) {
    switch (rotation) {
    case 90:
        ox = -y;
        oy = x;
        break;
    case 180:
        ox = -x;
        oy = -y;
        break;
    case 270:
        ox = y;
        oy = -x;
        break;
    default:
        ox = x;
        oy = y;
        break;
    }
}

} // namespace tb::render
