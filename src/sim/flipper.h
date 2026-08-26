#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>

// Flippers (08-physics.md §5): tapered-capsule geometry, stroke state
// machine, and the moving-capsule CCD data.
namespace tb::sim {

inline constexpr float kPi = 3.14159265358979f;

enum class FlipperState : uint8_t { Rest = 0, Rising = 1, Hold = 2, Dropping = 3 };

struct FlipperParams {
    Vec2 pivot{};
    float length = 0.076f; // m
    float radius_base = 0.011f;
    float radius_tip = 0.007f;
    float rest_angle_deg = -31.0f;
    float swing_deg = 52.0f;
    int side_sign = 1;   // +1 left (CCW up-swing), -1 right
    uint16_t action = 0; // §9.1 action index driving this flipper
    float strength = 1.0f;

    float swing_rad() const { return swing_deg * (float(kPi) / 180.0f); }

    float rest_rad() const { return rest_angle_deg * (float(kPi) / 180.0f); }
};

// Global stroke constants (08 §5.5).
constexpr float kOmegaMax = 42.0f;             // rad/s at strength 1
constexpr float kTauRise = 0.011f;             // s ramp-up
constexpr float kAlphaDrop = 2400.0f;          // rad/s²
constexpr float kOmegaDropMax = 24.0f;         // rad/s release cap
constexpr float kMovingOmegaFastPath = 1.0f;   // below: static swept path
constexpr uint32_t kLiveCatchWindowTicks = 70; // §1.3 default (50 ms)
constexpr float kLiveCatchFactor = 0.15;       // §1.3 default

// Per-tick kinematic state: ω held constant within the tick (step 2),
// θ advances after CCD used it (§5.2).
struct Flipper {
    FlipperParams params{};
    FlipperState state = FlipperState::Rest;
    float progress = 0.0f;                  // p ∈ [0,1]
    float theta = 0.0f;                     // current world angle (rad)
    float omega = 0.0f;                     // this tick's angular velocity (rad/s)
    float theta_start = 0.0f;               // θ0 when CCD began this tick
    bool enabled = true;                    // tb.set_flipper_enabled / tilt
    bool button_latched = false;            // logical level used this tick
    uint32_t ticks_since_eos = 0xFFFFFFFFu; // live-catch window clock
    uint32_t rise_ticks = 0;                // time since RISING entry (τ_rise ramp)
    float drop_omega = 0.0f;                // |ω| accumulated during DROPPING

    float angle_at(float t_sec) const { return theta_start + omega * t_sec; }
};

class FlipperSim {
public:
    // Advances one tick; sets out.theta_start/omega for CCD consumption.
    static void tick(Flipper& f, bool pressed);
};

} // namespace tb::sim
