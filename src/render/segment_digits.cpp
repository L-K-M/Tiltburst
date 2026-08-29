#include "render/segment_digits.h"

#include <cmath>

namespace tb::render {

namespace {

// §14.2 segment endpoints in cell-normalized [0,1] (y up).
struct Seg {
    float ax, ay, bx, by;
};

constexpr Seg kSegs[14] = {
    {0.10f, 0.97f, 0.90f, 0.97f}, // A  bit0
    {0.95f, 0.92f, 0.95f, 0.55f}, // B  bit1
    {0.95f, 0.45f, 0.95f, 0.08f}, // C  bit2
    {0.10f, 0.03f, 0.90f, 0.03f}, // D  bit3
    {0.05f, 0.45f, 0.05f, 0.08f}, // E  bit4
    {0.05f, 0.92f, 0.05f, 0.55f}, // F  bit5
    {0.10f, 0.50f, 0.45f, 0.50f}, // G1 bit6
    {0.55f, 0.50f, 0.90f, 0.50f}, // G2 bit7
    {0.13f, 0.90f, 0.42f, 0.57f}, // H  bit8
    {0.50f, 0.92f, 0.50f, 0.55f}, // I  bit9
    {0.87f, 0.90f, 0.58f, 0.57f}, // J  bit10
    {0.13f, 0.10f, 0.42f, 0.43f}, // K  bit11
    {0.50f, 0.45f, 0.50f, 0.08f}, // L  bit12
    {0.87f, 0.10f, 0.58f, 0.43f}, // M  bit13
};

} // namespace

void SegmentDigits::emit(char c,
                         float x,
                         float y,
                         float w,
                         float h,
                         bool lit,
                         float color_r,
                         float color_g,
                         float color_b,
                         std::vector<QuadInstance>* out) {
    if (c == ',') {
        // §14.2: small capsule below the baseline right of the cell.
        const float brightness = lit ? 1.0f : kGhost;
        QuadInstance q;
        q.cx = x + w * 0.85f;
        q.cy = y - h * 0.08f;
        q.hx = w * 0.06f;
        q.hy = h * 0.03f;
        q.r = color_r * brightness;
        q.g = color_g * brightness;
        q.b = color_b * brightness;
        q.a = brightness;
        out->push_back(q);
        return;
    }
    if (c == ' ') {
        return; // blank cell
    }

    const uint16_t mask = kDigitMask(c);
    if (mask == 0) {
        return; // non-digit: the caller falls back to atlas text
    }

    const float brightness = lit ? 1.0f : kGhost;
    const float thickness = kThickness * w; // px

    for (int b = 0; b < 14; ++b) {
        if (((mask >> b) & 1u) == 0) {
            continue;
        }
        const Seg& sgn = kSegs[size_t(b)];
        // Cell-normalized → cell px, y-up (the quad pipeline flips).
        float ax = sgn.ax * w;
        float ay = sgn.ay * h;
        float bx = sgn.bx * w;
        float by = sgn.by * h;
        // Italic skew BEFORE positioning (§14.2).
        ax += kItalic * sgn.ay * w;
        bx += kItalic * sgn.by * w;

        // Capsule center + half-extents.
        const float cx = x + (ax + bx) * 0.5f;
        const float cy = y + (ay + by) * 0.5f;
        const float dx = bx - ax, dy = by - ay;
        // Quad fallback: the TIGHT axis-aligned bounding box of the
        // oriented capsule (|dx| + r by |dy| + r), not a square of
        // the diagonal — until the backglass moves to the SDF
        // pipeline (§14.2's pipeline 13) for true capsules.
        QuadInstance q;
        q.cx = cx;
        q.cy = cy;
        q.hx = std::fabs(dx) * 0.5f + thickness * 0.5f;
        q.hy = std::fabs(dy) * 0.5f + thickness * 0.5f;
        q.r = color_r * brightness;
        q.g = color_g * brightness;
        q.b = color_b * brightness;
        q.a = brightness;
        out->push_back(q);
    }
}

} // namespace tb::render
