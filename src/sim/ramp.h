#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>
#include <vector>

// Ramp 1-D constraint (08-physics.md §6.10) and magnet field (§6.12).
namespace tb::sim {

// Baked ramp: path flattened to arc-length parametrized points with the
// height profile resampled per point. Sized at build; the hot path only
// reads it.
struct RampPath {
    struct Sample {
        Vec2 pos{};     // Γ(s)
        Vec2 tangent{}; // t̂(s), unit, toward increasing s
        float s = 0.0f; // arc length at this sample
        float z = 0.0f; // height above layer 0
    };

    std::vector<Sample> samples; // s strictly increasing, [0] = s 0
    float total_s = 0.0f;        // S
    float width = 0.044f;
    bool drop_exit = false;
    uint16_t element_id = 0xFFFF;

    // Derived seam layers (§6.10.2): 0, 1, or 0xFF for an internal end
    // (drop_exit far end carries no seam).
    uint8_t seam_layer[2] = {0xFF, 0xFF};
    float end_z[2] = {0.0f, 0.0f};
    Vec2 seam_center[2] = {};
    Vec2 seam_into[2] = {}; // into-path tangent at each end

    Vec2 point_at(float s) const;
    Vec2 tangent_at(float s) const;
    float z_at(float s) const;
};

struct MagnetSim {
    Vec2 pos{};
    float radius = 0.09f;
    float strength = 1.2f;
    uint8_t layer = 0;
    bool on = false;
    uint32_t pulse_ticks_left = 0; // pulse envelope remaining
    uint32_t pulse_total = 0;      // pulse duration (for the 0.6/0.4 envelope)

    // §6.12: field acceleration for a ball inside the enabled field;
    // layer match and ball freedom (not held/locked) are caller-gated.
    Vec2 accel(const Ball& ball) const;
    // §6.12 damping factors while inside the enabled field.
    void damp(Ball& ball) const;

    void set_active(bool active) {
        on = active;
        pulse_ticks_left = 0;
        pulse_total = 0;
    }

    void pulse(uint32_t ticks) {
        if (ticks == 0) {
            return; // zero-duration pulse must not latch the field on
        }
        pulse_ticks_left = ticks;
        pulse_total = ticks;
        on = true;
    }
};

} // namespace tb::sim
