#include "sim/collider.h"
#include "sim/flipper.h"
#include "sim/solver.h"

#include <memory>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

// FT-01..FT-08 feel scenarios (08-physics.md §5.7, tagged M4), run on the
// normative §5.6 code rig: seed 0x54425354 identity, state-triggered
// inputs, no table.json, no replay tape.
namespace ft {

using tb::sim::Vec2;
using tb::sim::Ball;
using tb::sim::BallMode;
using tb::sim::Collider;
using tb::sim::Flipper;
using tb::sim::MaterialId;
using tb::sim::TickInput;
using tb::sim::kBallRadius;

struct Rig {
    tb::sim::SimState state;
    tb::sim::Solver solver;
    int left = 0;  // flipper indices
    int right = 1;
    uint32_t buttons = 0; // current mask: bit0 left_flipper, bit1 right
};

std::unique_ptr<Rig> make_rig() {
    auto rig = std::make_unique<Rig>();
    tb::sim::SimState& s = rig->state;
    s.slope_deg = 6.5f;
    s.width = 0.52f;
    s.height = 1.04f;

    uint16_t sub = 0;
    auto wall = [&](Vec2 a, Vec2 b) {
        tb::sim::Collider c{};
        c.kind = tb::sim::Collider::Kind::Segment;
        c.a = a;
        c.b = b;
        c.element_id = 100;
        c.sub_index = sub++;
        c.material = tb::sim::MaterialId::Wood;
        s.colliders.push_back(c);
    };
    auto post = [&](Vec2 p) {
        tb::sim::Collider c{};
        c.kind = tb::sim::Collider::Kind::Point;
        c.a = p;
        c.radius = 0.008f;
        c.element_id = 101;
        c.sub_index = sub++;
        c.material = tb::sim::MaterialId::Rubber;
        s.colliders.push_back(c);
    };

    wall({0.0f, 0.0f}, {0.52f, 0.0f});   // border
    wall({0.52f, 0.0f}, {0.52f, 1.04f});
    wall({0.52f, 1.04f}, {0.0f, 1.04f});
    wall({0.0f, 1.04f}, {0.0f, 0.0f});
    wall({0.148f, 0.300f}, {0.166f, 0.140f}); // inlane guides (§5.6)
    wall({0.372f, 0.300f}, {0.354f, 0.140f});
    post({0.148f, 0.300f});
    post({0.372f, 0.300f});
    wall({0.20f, 0.015f}, {0.32f, 0.015f});   // outhole line

    tb::sim::Flipper fl{};
    fl.params.pivot = {0.170f, 0.120f};
    fl.params.rest_angle_deg = -31.0f;
    fl.params.swing_deg = 52.0f;
    fl.params.side_sign = +1;
    fl.params.action = 0;
    fl.theta = fl.params.rest_rad();
    fl.theta_start = fl.theta;
    s.flippers.push_back(fl);

    tb::sim::Flipper fr = fl;
    fr.params.pivot = {0.350f, 0.120f};
    fr.params.rest_angle_deg = 211.0f;
    fr.params.side_sign = -1;
    fr.params.action = 1;
    fr.theta = fr.params.rest_rad();
    fr.theta_start = fr.theta;
    s.flippers.push_back(fr);

    s.grid.build(s.colliders, s.width, s.height);
    return rig;
}

void spawn(Rig& rig, float x, float y, float vx, float vy) {
    Ball& b = rig.state.balls[0];
    b.index = 0;
    b.live = true;
    b.mode = tb::sim::BallMode::Free;
    b.layer = 0;
    b.pos = {x, y};
    b.vel = {vx, vy};
    b.last_safe_pos = b.pos;
}

float speed(const Rig& rig) { return length(rig.state.balls[0].vel); }

bool drained(const Rig& rig) {
    const Ball& b = rig.state.balls[0];
    return !b.live || (b.pos.y < 0.02f && b.pos.x > 0.19f && b.pos.x < 0.33f);
}

// §5.6 cradled(): |v| < 0.05 m/s and ball within 2 mm of the surface.
// Approximated live against the capsule axis at the current angle.
bool cradled(const Rig& rig, int idx) {
    const Ball& b = rig.state.balls[0];
    if (!b.live || length_sq(b.vel) > 0.05 * 0.05) {
        return false;
    }
    const Flipper& f = rig.state.flippers[size_t(idx)];
    const Vec2 axis{std::cos(f.theta), std::sin(f.theta)};
    const float along =
        std::clamp(dot(b.pos - f.params.pivot, axis), 0.0f, f.params.length);
    const Vec2 closest = f.params.pivot + axis * along;
    // Capsule radius tapers linearly base->tip (§5.1).
    const float cap_r =
        f.params.radius_base +
        (f.params.radius_tip - f.params.radius_base) *
            (along / std::max(f.params.length, 1e-6f));
    return length(b.pos - closest) <
           cap_r + kBallRadius + 0.002f; // §5.6: < 2 mm gap
}

void step(Rig& rig, bool left, bool right) {
    tb::sim::TickInput in;
    in.buttons = (left ? 1u : 0u) | (right ? 2u : 0u);
    rig.buttons = in.buttons;
    rig.solver.step(rig.state, in);
}

void run(Rig& rig, int ms, bool left, bool right) {
    for (int i = 0; i < ms; ++i) {
        step(rig, left, right);
    }
}

// CRADLE_SETUP (§5.6): spawn + press + hold; must cradle within 1.5 s.
void cradle_setup(Rig& rig) {
    spawn(rig,  0.185f, 0.165f, 0.0f, 0.0f);
    run(rig, 1500, true, false);
    ASSERT_TRUE(cradled(rig, 0)) << "CRADLE_SETUP failed";
}

// Distance from the LEFT pivot to the ball's nearest capsule surface.
float rho_left(const Rig& rig) {
    const Ball& b = rig.state.balls[0];
    const Flipper& f = rig.state.flippers[0];
    return length(b.pos - f.params.pivot);
}

} // namespace ft

namespace {

using ft::cradle_setup;
using ft::make_rig;
using ft::run;
using ft::spawn;
using tb::sim::Flipper;
using tb::sim::Vec2;
using std::cos;
using std::sin;
using std::fabs;
using std::clamp;

constexpr float kPiF = 3.14159265358979f / 180.0f;

// Distance from the LEFT flipper surface to the ball center.
float surface_dist(const ft::Rig& rig) {
    const tb::sim::Ball& b = rig.state.balls[0];
    const tb::sim::Flipper& f = rig.state.flippers[0];
    const tb::sim::Vec2 axis{std::cos(f.theta), std::sin(f.theta)};
    const float along = std::clamp(
        tb::sim::dot(b.pos - f.params.pivot, axis), 0.0f, f.params.length);
    const tb::sim::Vec2 closest = f.params.pivot + axis * along;
    return length(b.pos - closest) -
           (tb::sim::kBallRadius + 0.009f); // avg rubber radius
}

// First contact event with anything while `left` is held, detected by a
// velocity discontinuity well above slope gravity's per-tick increment.
// Returns the along-axis parameter on the LEFT flipper at that moment,
// or -1 if none / drained.
float first_contact_rho(ft::Rig& rig, int max_ms, bool left) {
    tb::sim::Vec2 prev = rig.state.balls[0].vel;
    for (int i = 0; i < max_ms; ++i) {
        run(rig, 1, left, false);
        const tb::sim::Vec2 v = rig.state.balls[0].vel;
        if (length(v - prev) > 0.02f) { // >> gravity per tick
            const tb::sim::Ball& b = rig.state.balls[0];
            const tb::sim::Flipper& f = rig.state.flippers[0];
            const tb::sim::Vec2 axis{std::cos(f.theta),
                                     std::sin(f.theta)};
            const float rho =
                std::clamp(tb::sim::dot(b.pos - f.params.pivot, axis),
                           0.0f, f.params.length);
            return rho;
        }
        prev = v;
        if (ft::drained(rig)) {
            return -1.0f;
        }
    }
    return -1.0f;
}

// Whether the ball crosses y_target within max_ms of free flight; fills
// speed/x at the crossing.
bool crosses_y(ft::Rig& rig, int max_ms, float y_target, float* speed_at,
               float* x_at) {
    for (int i = 0; i < max_ms; ++i) {
        run(rig, 1, false, false);
        if (rig.state.balls[0].pos.y >= y_target) {
            if (speed_at != nullptr) {
                *speed_at = ft::speed(rig);
            }
            if (x_at != nullptr) {
                *x_at = rig.state.balls[0].pos.x;
            }
            return true;
        }
        if (ft::drained(rig)) {
            return false;
        }
    }
    return false;
}

} // namespace

// FT-01 Dead bounce: falls onto the resting left flipper; rebounds toward
// the right side, reaching x ≥ 0.28 m without re-touching.
TEST(feel_scenarios, ft01_dead_bounce) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    spawn(rig,  0.205f, 0.220f, 0.0f, -1.2f);

    ASSERT_GE(first_contact_rho(rig, 500, false), 0.0f)
        << "no flipper contact";

    run(rig, 10, false, false); // measure 10 ms after first contact
    const float rebound = ft::speed(rig);
    EXPECT_GE(rebound, 0.75f);
    EXPECT_LE(rebound, 1.15f);
    EXPECT_GT(rig.state.balls[0].vel.x, 0.4f);

    bool crossed = false;
    for (int i = 0; i < 400 && !crossed; ++i) {
        run(rig, 1, false, false);
        crossed = rig.state.balls[0].pos.x >= 0.28f;
    }
    EXPECT_TRUE(crossed);
}

// State-triggered press helper: presses when predicate fires, holds.
// Returns the tick (ms) of the press.

namespace {

struct PressScript {
    ft::Rig* rig = nullptr;
    bool pressed = false;
    bool (*when)(const tb::sim::SimState&) = nullptr;
    int release_after_ms = -1; // -1 = hold forever
    int held_ms = 0;

    bool step() {
        if (!pressed && when != nullptr && when(rig->state)) {
            pressed = true;
        }
        const bool hold_now =
            pressed && (release_after_ms < 0 || held_ms < release_after_ms);
        if (pressed) {
            ++held_ms;
        }
        rig->buttons = hold_now ? 1u : 0u;
        tb::sim::TickInput in;
        in.buttons = rig->buttons;
        rig->solver.step(rig->state, in);
        return pressed;
    }
};

} // namespace

// FT-02 Live catch: press left when ball.y ≤ 0.26, hold; ball dies on the
// just-raised flipper.
TEST(feel_scenarios, ft02_live_catch) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    spawn(rig, 0.205f, 0.50f, 0.0f, -2.5f);

    PressScript press;
    press.rig = &rig;
    press.when = [](const tb::sim::SimState& s) { return s.balls[0].pos.y <= 0.26f; };

    int contact_ms = -1;
    float prev_speed = ft::speed(rig);
    for (int i = 0; i < 2000 && contact_ms < 0; ++i) {
        press.step();
        const float s = ft::speed(rig);
        if (press.pressed && std::fabs(s - prev_speed) > 0.05f) {
            contact_ms = i;
        }
        prev_speed = s;
    }
    ASSERT_GE(contact_ms, 0) << "no contact";
    EXPECT_EQ(rig.state.flippers[0].state, tb::sim::FlipperState::Hold);

    run(rig, 150, true, false); // within 150 ms: |v| < 0.40
    EXPECT_LT(ft::speed(rig), 0.40f);

    run(rig, 350, true, false); // by 0.5 s
    for (int i = 0; i < 1500; ++i) { // 0.5..2.0 s: caught
        run(rig, 1, true, false);
        ASSERT_LT(ft::speed(rig), 0.10f) << "t=" << i;
        ASSERT_FALSE(ft::drained(rig));
    }
}

// FT-03 Cradle hold: CRADLE_SETUP, hold 3 s; < 1 mm drift over [2,3] s.
TEST(feel_scenarios, ft03_cradle_hold) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    cradle_setup(rig);

    for (int i = 0; i < 500; ++i) { // 1.5 → 2.0 s
        run(rig, 1, true, false);
        EXPECT_TRUE(cradled(rig, 0));
    }
    const Vec2 at2s = rig.state.balls[0].pos;
    run(rig, 1000, true, false); // 2.0 → 3.0 s
    EXPECT_LT(length(rig.state.balls[0].pos - at2s), 0.001f);
    EXPECT_LT(rig.state.balls[0].pos.x, 0.235f);
}

// FT-04 Backhand: cradle, release at 2.0 s, re-press at +50 ms and hold.
TEST(feel_scenarios, ft04_backhand) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    cradle_setup(rig);

    run(rig, 50, false, false);      // release
    bool crossed = false;
    for (int i = 0; i < 1000 && !crossed; ++i) {
        run(rig, 1, true, false);    // re-press and hold
        if (rig.state.balls[0].pos.y >= 0.85f) {
            crossed = true;
            EXPECT_GE(ft::speed(rig), 1.8f);
            const float x = rig.state.balls[0].pos.x;
            EXPECT_GE(x, 0.12f);
            EXPECT_LE(x, 0.28f);
        }
    }
    ASSERT_TRUE(crossed) << "ball never reached y = 0.85";
}

// FT-05 Post pass: low arc to the raised right flipper.
TEST(feel_scenarios, ft05_post_pass) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    cradle_setup(rig);

    run(rig, 70, false, false);   // release at 2.0 s for 70 ms
    run(rig, 80, true, false);    // re-press left at 2.070, hold 80 ms
    run(rig, 0, false, true);     // release left, press right at 2.150

    float y_max = 0.0f;
    bool settled = false;
    for (int i = 0; i < 2500 && !settled; ++i) {
        run(rig, 1, false, true); // right held
        y_max = std::max(y_max, rig.state.balls[0].pos.y);
        if (i > 200 && cradled(rig, 1)) {
            settled = true;
        }
        ASSERT_FALSE(drained(rig));
    }
    EXPECT_GE(y_max, 0.18f);
    EXPECT_LE(y_max, 0.48f);
    EXPECT_TRUE(settled) << "ball never settled on the right flipper";
}

// FT-06 Tap pass: gentle lob transfer.
TEST(feel_scenarios, ft06_tap_pass) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    cradle_setup(rig);

    run(rig, 30, false, false);   // release 30 ms
    run(rig, 50, true, false);    // tap left 50 ms
    run(rig, 40, false, false);   // gap
    run(rig, 0, false, true);     // press right at 2.120 and hold

    float y_max = 0.0f;
    bool settled = false;
    float exit_speed = -1.0f;
    bool lefted = false;
    for (int i = 0; i < 2500 && !settled; ++i) {
        run(rig, 1, false, true);
        y_max = std::max(y_max, rig.state.balls[0].pos.y);
        if (!lefted && i > 60) {
            exit_speed = speed(rig);
            lefted = true;
        }
        if (i > 200 && cradled(rig, 1)) {
            settled = true;
        }
        ASSERT_FALSE(drained(rig));
    }
    EXPECT_LE(exit_speed, 2.0f);
    EXPECT_LE(y_max, 0.32f);
    EXPECT_TRUE(settled);
}

// FT-07 Tip shot power: press when ball.y ≤ 0.175; contact at ρ ≥ 55 mm;
// exit in [4.5, 8.5] m/s crossing y=0.95 within 0.5 s.
TEST(feel_scenarios, ft07_tip_shot_power) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    spawn(rig,  0.240f, 0.55f, 0.0f, -2.0f);

    PressScript press;
    press.rig = &rig;
    press.when = [](const tb::sim::SimState& s) {
        return s.balls[0].pos.y <= 0.175f;
    };
    press.release_after_ms = 100;

    int contact_ms = -1;
    float prev_speed = speed(rig);
    for (int i = 0; i < 1500 && contact_ms < 0; ++i) {
        press.step();
        const float s = speed(rig);
        if (press.pressed && fabs(s - prev_speed) > 0.10f) {
            contact_ms = i;
        }
        prev_speed = s;
    }
    ASSERT_GE(contact_ms, 0) << "no contact";

    // Contact rho from the pivot.
    const Flipper& f = rig.state.flippers[0];
    const tb::sim::Vec2 axis{std::cos(f.theta), std::sin(f.theta)};
    const float rho =
        std::clamp(length(rig.state.balls[0].pos - f.params.pivot), 0.0f, f.params.length);
    EXPECT_GE(rho, 0.055f);

    run(rig, 10, false, false); // exit speed 10 ms after contact
    const float exit = speed(rig);
    EXPECT_GE(exit, 4.5f);
    EXPECT_LE(exit, 8.5f);

    bool crossed = false;
    for (int i = 0; i < 500 && !crossed; ++i) {
        run(rig, 1, false, false);
        crossed = rig.state.balls[0].pos.y >= 0.95f;
    }
    EXPECT_TRUE(crossed);
}

// FT-08 Cradle escape via slap: release, ball rolls down; slap at 140 ms.
TEST(feel_scenarios, ft08_cradle_escape_slap) {
    std::unique_ptr<ft::Rig> rig_owner = make_rig();
    ft::Rig& rig = *rig_owner;
    cradle_setup(rig);

    run(rig, 140, false, false); // roll down the flipper

    bool slapped = false;
    for (int i = 0; i < 1000 && !slapped; ++i) {
        run(rig, 1, true, false);
        if (speed(rig) > 1.0f && i > 100) { // struck and leaving
            slapped = true;
        }
        if (drained(rig)) {
            FAIL() << "ball drained during escape";
        }
    }
    ASSERT_TRUE(slapped);

    const Flipper& f = rig.state.flippers[0];
    const tb::sim::Vec2 axis{std::cos(f.theta), std::sin(f.theta)};
    const float rho =
        std::clamp(tb::sim::dot(rig.state.balls[0].pos - f.params.pivot, axis), 0.0f, f.params.length);
    EXPECT_GE(rho, 0.04f);

    const float exit = speed(rig);
    EXPECT_GE(exit, 3.0f);
    EXPECT_LE(exit, 7.5f);

    bool high = false;
    for (int i = 0; i < 1000 && !high; ++i) {
        run(rig, 1, false, false);
        high = rig.state.balls[0].pos.y >= 0.70f;
        ASSERT_FALSE(drained(rig));
    }
    EXPECT_TRUE(high);
}
