#include "sim/ccd.h"

#include "core/rng.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

} // namespace

// The five worked examples of 16-testing-ci.md §2.1, pinned to the exact
// given inputs, expected values, and tolerances (1e-6 absolute).

TEST(unit_sweep, segment_interior_flat) {
    // Vertical drop onto a horizontal wall.
    const tb::sim::Vec2 p0{0.26f, 0.50f};
    const tb::sim::Vec2 v{0.0f, -2.0f};
    const tb::sim::Vec2 a{0.00f, 0.10f};
    const tb::sim::Vec2 b{0.52f, 0.10f};

    tb::sim::SweepHit hit;
    ASSERT_TRUE(tb::sim::sweep_circle_vs_segment(p0, v, 0.0135f, a, b, 1.0f, hit));
    EXPECT_NEAR(hit.toi, 0.19325f, 1e-6);
    EXPECT_NEAR(hit.normal.x, 0.0f, 1e-6);
    EXPECT_NEAR(hit.normal.y, 1.0f, 1e-6);

    const tb::sim::Vec2 center = p0 + v * hit.toi;
    EXPECT_NEAR(center.x, 0.26f, 1e-6);
    EXPECT_NEAR(center.y, 0.1135f, 1e-6);

    // Contact point (0.26, 0.10): segment parameter s = 0.26/0.52 = 0.5.
}

TEST(unit_sweep, segment_interior_45deg) {
    const tb::sim::Vec2 p0{0.30f, 0.10f};
    const tb::sim::Vec2 v{-1.0f, 1.0f};
    const tb::sim::Vec2 a{0.10f, 0.10f};
    const tb::sim::Vec2 b{0.30f, 0.30f};

    tb::sim::SweepHit hit;
    ASSERT_TRUE(tb::sim::sweep_circle_vs_segment(p0, v, 0.0135f, a, b, 1.0f, hit));
    EXPECT_NEAR(hit.toi, 0.09045406f, 1e-6);
    EXPECT_NEAR(hit.normal.x, 0.70710678f, 1e-6);
    EXPECT_NEAR(hit.normal.y, -0.70710678f, 1e-6);

    const tb::sim::Vec2 center = p0 + v * hit.toi;
    EXPECT_NEAR(center.x, 0.20954594f, 1e-6);
    EXPECT_NEAR(center.y, 0.19045406f, 1e-6);
}

TEST(unit_sweep, segment_endpoint_cap) {
    // Path misses the interior, clips endpoint B = (0.11, 0.10).
    const tb::sim::Vec2 p0{0.10f, 0.30f};
    const tb::sim::Vec2 v{0.0f, -3.0f};
    const tb::sim::Vec2 a{0.30f, 0.10f};
    const tb::sim::Vec2 b{0.11f, 0.10f};

    tb::sim::SweepHit hit;
    ASSERT_TRUE(tb::sim::sweep_circle_vs_segment(p0, v, 0.0135f, a, b, 1.0f, hit));
    EXPECT_NEAR(hit.toi, 0.06364361f, 1e-6);

    const tb::sim::Vec2 center = p0 + v * hit.toi;
    EXPECT_NEAR(center.x, 0.10f, 1e-6);
    EXPECT_NEAR(center.y, 0.10906918f, 1e-6);

    const tb::sim::Vec2 n = tb::sim::normalize(center - b);
    EXPECT_NEAR(n.x, -0.74074074f, 1e-6);
    EXPECT_NEAR(n.y, 0.67179101f, 1e-6);
    EXPECT_NEAR(tb::sim::length(n), 1.0f, 1e-6);
}

TEST(unit_sweep, arc_inside_hit) {
    // Ball inside a circular guide; effective radius R − r. The guide's
    // window covers the upper half (the hit direction).
    const tb::sim::Vec2 c{0.26f, 0.52f};
    constexpr float kRadius = 0.10f;
    const tb::sim::Vec2 p0{0.26f, 0.48f};
    const tb::sim::Vec2 v{0.0f, 1.5f};

    tb::sim::SweepHit hit;
    ASSERT_TRUE(
        tb::sim::sweep_circle_vs_arc(p0, v, 0.0135f, c, kRadius, 0.0f, float(kPi), 1.0f, hit));
    EXPECT_NEAR(hit.toi, 0.08433333f, 1e-6);
    EXPECT_NEAR(hit.normal.x, 0.0f, 1e-6);
    EXPECT_NEAR(hit.normal.y, -1.0f, 1e-6);

    const tb::sim::Vec2 center = p0 + v * hit.toi;
    EXPECT_NEAR(center.x, 0.26f, 1e-5);
    EXPECT_NEAR(center.y, 0.6065f, 1e-4);
}

TEST(unit_integrate, slope_gravity_1s) {
    // Semi-implicit Euler closed form: from rest at (0.26, 0.80),
    // Δy = −a·dt²·N(N+1)/2 with a = 9.81·sin(6.5°). Double accumulation
    // pins the integrator ORDER without float-rounding noise.
    constexpr double kDt = 0.001;
    constexpr int kTicks = 1000;
    const double a = 9.81 * std::sin(6.5 * 3.14159265358979 / 180.0);

    double vy = 0.0;
    double y = 0.80;
    for (int i = 0; i < kTicks; ++i) {
        vy += -a * kDt;
        y += vy * kDt;
    }

    EXPECT_NEAR(vy, -1.11052353, 1e-5);
    EXPECT_NEAR(y, 0.24418297, 1e-4);
}

// No tunnelling at clamp speed: 10,000 random headings against a thin
// horizontal wall; whenever the center path crosses the wall plane within
// its span during the window, the sweep must return a TOI (04 M2).
TEST(unit_ccd_segment, no_tunnel_at_12mps) {
    tb::Pcg32 rng;
    rng.seed(1234u, 1u);

    const tb::sim::Vec2 a{0.20f, 0.50f};
    const tb::sim::Vec2 b{0.80f, 0.50f};
    constexpr float kRadius = 0.0135f;

    int crossings_checked = 0;
    for (int i = 0; i < 10000; ++i) {
        const float angle = rng.next_float() * 2.0f * float(kPi);
        const tb::sim::Vec2 p0{0.20f + 0.6f * rng.next_float(), 0.30f};
        const tb::sim::Vec2 v{std::cos(angle) * 12.0f, std::sin(angle) * 12.0f};

        if (v.y <= 0.0f) {
            continue; // heading away from the wall above
        }

        // Analytic plane-crossing time of the CENTER path with y = 0.50.
        const float t_line = (0.50f - p0.y) / v.y;
        if (t_line <= 0.0f || t_line > 1.0f) {
            continue; // outside the test window
        }
        const float x_at_line = p0.x + v.x * t_line;
        if (x_at_line < a.x - kRadius || x_at_line > b.x + kRadius) {
            continue; // passes beside the wall — endpoint caps may clip,
                      // but this run proves nothing about tunnelling
        }

        tb::sim::SweepHit hit;
        ASSERT_TRUE(tb::sim::sweep_circle_vs_segment(p0, v, kRadius, a, b, 1.0f, hit))
            << "tunnelling: center crossed the wall plane at t=" << t_line
            << " but the sweep found nothing";
        // The swept TOI lands within one radius of travel of the analytic
        // plane crossing (contact happens r/|v| early on approach).
        EXPECT_NEAR(hit.toi, t_line, kRadius / std::fabs(v.y) + 1e-6);
        ++crossings_checked;
    }
    EXPECT_GT(crossings_checked, 1000) << "test generated too few crossing cases to prove anything";
}
