#pragma once

#include "sim/ccd.h"
#include "sim/flipper.h"
#include "sim/math.h"

// Swept circle vs the rotating tapered capsule (08-physics.md §3.5).
namespace tb::sim {

struct FlipperHit {
    float toi = 0.0f;
    Vec2 normal{};      // surface -> ball center
    Vec2 contact{};     // X = P_c − r·n̂
    Vec2 surface_vel{}; // V_s = ω · perp(X − pivot)
};

// Conservative advancement when |ω| ≥ 1 rad/s; exact static swept path
// (caps + tangent side segments) below that.
bool sweep_circle_vs_flipper(
    Vec2 p0, Vec2 v, float r, const Flipper& f, float max_t, FlipperHit& out);

// Discrete separation probe at angle theta: signed distance from the ball
// center to the capsule surface, the world-space branch normal (surface →
// center), and the surface velocity at the closest surface point. Used by
// the solver's persistent-contact path for resting/sliding balls.
struct FlipperSep {
    float sep = 0.0f;
    Vec2 normal{};
    Vec2 surface_vel{};
};

FlipperSep flipper_separation(Vec2 p, float r, const Flipper& f, float theta);

} // namespace tb::sim
