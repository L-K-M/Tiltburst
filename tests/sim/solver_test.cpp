#include "sim/solver.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

// A flat table (no gravity) with the synthetic border: every contact is
// passive (surface velocity zero), so kinetic energy may only decay.
// SimState is non-copyable (atomics), so callers own their instance.
void make_flat_scene(tb::sim::SimState& s, uint64_t seed) {
    tb::sim::make_synthetic_scene(s, seed);
    s.slope_deg = 0.0f;
    for (int i = 0; i < tb::sim::kMaxBalls; ++i) {
        s.balls[i].vel = s.balls[i].vel * 0.25f;
    }
}

double total_ke(const tb::sim::SimState& s) {
    double ke = 0.0;
    for (int i = 0; i < tb::sim::kMaxBalls; ++i) {
        const tb::sim::Ball& b = s.balls[i];
        if (!b.live) {
            continue;
        }
        ke += 0.5 * 0.08 * (double(b.vel.x) * b.vel.x + double(b.vel.y) * b.vel.y) +
              0.5 * 5.832e-6 * double(b.omega_z) * b.omega_z;
    }
    return ke;
}

} // namespace

// Energy.PassiveBounceNeverGains: with gravity off, all contacts are
// passive, so kinetic energy may only decay (08-physics.md §4.4; the CI
// fuzz invariant budgets the float-rounding slack at 1e-6 J per tick).
TEST(unit_energy, passive_bounce_never_gains) {
    tb::sim::SimState s;
    make_flat_scene(s, 424242);

    double prev = total_ke(s);
    ASSERT_GT(prev, 0.0);

    tb::sim::Solver solver;
    const tb::sim::TickInput input;
    for (int tick = 0; tick < 20000; ++tick) {
        solver.step(s, input);
        const double now = total_ke(s);
        ASSERT_LE(now - prev, 1e-6) << "tick " << tick << " gained " << (now - prev) << " J";
        prev = now;
    }
}

// CcdBallBall.HeadOnMomentum: equal-mass head-on hit conserves momentum
// and separates the balls (08-physics.md §8).
TEST(unit_ccd_ball_ball, head_on_momentum) {
    tb::sim::SimState s;
    s.width = 1.0f;
    s.height = 2.0f;
    s.slope_deg = 0.0f;
    s.mu_rr = 0.0f; // isolate pair impulse from dissipation
    s.air_drag = 0.0f;
    s.grid.build(s.colliders, s.width, s.height);

    tb::sim::Ball& a = s.balls[0];
    tb::sim::Ball& b = s.balls[1];
    a.index = 0;
    b.index = 1;
    a.live = true;
    b.live = true;
    a.pos = {0.40f, 1.00f};
    b.pos = {0.60f, 1.00f};
    a.vel = {3.0f, 0.0f};
    b.vel = {0.0f, 0.0f};
    a.last_safe_pos = a.pos;
    b.last_safe_pos = b.pos;

    tb::sim::Solver solver;
    for (int i = 0; i < 400; ++i) {
        const tb::sim::TickInput input;
        solver.step(s, input);
    }

    // Momentum is conserved exactly by the ±impulse pair; restitution only
    // splits it between the balls.
    EXPECT_NEAR(double(a.vel.x) + double(b.vel.x), 3.0, 1e-3);
    EXPECT_GT(b.vel.x, a.vel.x); // they separated again
    EXPECT_LT(a.vel.x, 3.0);
}
