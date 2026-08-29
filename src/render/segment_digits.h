#pragma once

#include "render/renderer.h"

#include <cstdint>
#include <vector>

namespace tb::render {

// 14-segment score digits (06-rendering.md §14.2). Builds capsule
// SdfInstances in pixel space: cell normalized [0,1]×[0,1] (y up),
// thickness 0.09, optional 8° italic skew (x += 0.14·y), glow radius
// 0.35 × cell width. Ghost (unlit) segments draw at 6% brightness,
// no glow.
class SegmentDigits {
public:
    // §14.2 bit masks (bit0=A .. 13=M).
    static constexpr uint16_t kMask[11] = {
        0x0000, // space
        0x0006, // 1
        0x00DB, // 2
        0x008F, // 3
        0x00E6, // 4
        0x00ED, // 5
        0x00FD, // 6
        0x0007, // 7
        0x00FF, // 8
        0x00EF, // 9
        0x0C3F, // 0
    };

    static constexpr uint16_t kDigitMask(char c) {
        return c >= '0' && c <= '9' ? kMask[size_t(c == '0' ? 10 : c - '0')] : 0;
    }

    static constexpr float kThickness = 0.09f;  // cell-normalized
    static constexpr float kGlowFactor = 0.35f; // × cell width
    static constexpr float kGhost = 0.06f;      // unlit brightness
    static constexpr float kItalic = 0.14f;     // x += kItalic * y
    static constexpr float kItalicDeg = 8.0f;

    // Appends capsule instances for one digit (or ghost row) at pixel
    // (x, y) with cell size (w, h). `lit` false draws all segments at
    // 6% with no glow. The comma (§14.2: small capsule below the
    // baseline right of the cell) draws when c == ','.
    static void emit(char c,
                     float x,
                     float y,
                     float w,
                     float h,
                     bool lit,
                     float color_r,
                     float color_g,
                     float color_b,
                     std::vector<QuadInstance>* capsule_quads);
};

} // namespace tb::render
