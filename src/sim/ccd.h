#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>

// Analytic swept tests (08-physics.md §3). Every test is continuous: the
// ball center travels P0 + v·t and the hit time must land inside
// (0 - kToiEps, max_t]. The contact normal points from the collider
// surface toward the ball center.
namespace tb::sim {

struct SweepHit {
    float toi = 0.0f;
    Vec2 normal{};
    uint32_t collider_id = 0xFFFFFFFFu;
};

// Segment A→B, both sides collidable, endpoint caps excluded (baked as
// separate point colliders).
bool sweep_circle_vs_segment(Vec2 p0, Vec2 v, float r, Vec2 a, Vec2 b, float max_t, SweepHit& out);

// Static point/post at C with effective radius rho (= r for a bare corner,
// r + post_radius for a post).
bool sweep_circle_vs_point(Vec2 p0, Vec2 v, float r, Vec2 c, float rho, float max_t, SweepHit& out);

// Arc (center, radius) spanning CCW from angle a0 to a1 (a1 > a0 after
// normalization; wraps past 2π allowed). Tests outer and inner surfaces.
bool sweep_circle_vs_arc(Vec2 p0,
                         Vec2 v,
                         float r,
                         Vec2 center,
                         float radius,
                         float a0,
                         float a1,
                         float max_t,
                         SweepHit& out);

// Moving circle vs moving circle (ball-ball, §8): relative motion with
// combined radius. Normal points from circle 1 toward circle 2's center…
// convention here: normal points toward body 1 (the caller passes its own
// ordering consistently).
bool sweep_circle_vs_circle(
    Vec2 p0, Vec2 v0, float r0, Vec2 p1, Vec2 v1, float r1, float max_t, SweepHit& out);

} // namespace tb::sim
