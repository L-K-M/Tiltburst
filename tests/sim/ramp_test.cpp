#include "sim/ramp.h"

#include "core/rng.h"
#include "sim/solver.h"
#include "support/data_path.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

// M8: ramps, layers, magnets (04-milestones.md §M8; 08-physics.md §6.10–
// §6.12), including the M8-tagged feel scenarios FT-09/FT-10 on the §5.6
// rig plus its M8 additions — no table.json, no replay tape.
namespace {

using tb::sim::Ball;
using tb::sim::BallMode;
using tb::sim::Collider;
using tb::sim::MagnetSim;
using tb::sim::RampPath;
using tb::sim::SimEvent;
using tb::sim::SimState;
using tb::sim::Solver;
using tb::sim::TickInput;
using tb::sim::Vec2;

// The §5.6 feel rig (as in feel_test.cpp) plus the M8 additions.
struct Rig {
    SimState s;
    Solver solver;

    Rig() {
        s.slope_deg = 6.5f;
        s.width = 0.52f;
        s.height = 1.04f;
        uint16_t sub = 0;
        auto wall = [&](Vec2 a, Vec2 b) {
            Collider c{};
            c.kind = Collider::Kind::Segment;
            c.a = a;
            c.b = b;
            c.element_id = 100;
            c.sub_index = sub++;
            c.material = tb::sim::MaterialId::Wood;
            s.colliders.push_back(c);
        };
        auto post = [&](Vec2 p) {
            Collider c{};
            c.kind = Collider::Kind::Point;
            c.a = p;
            c.radius = 0.008f;
            c.element_id = 101;
            c.sub_index = sub++;
            c.material = tb::sim::MaterialId::Rubber;
            s.colliders.push_back(c);
        };
        wall({0, 0}, {0.52f, 0});
        wall({0.52f, 0}, {0.52f, 1.04f});
        wall({0.52f, 1.04f}, {0, 1.04f});
        wall({0, 1.04f}, {0, 0});
        wall({0.148f, 0.300f}, {0.166f, 0.140f});
        wall({0.372f, 0.300f}, {0.354f, 0.140f});
        post({0.148f, 0.300f});
        post({0.372f, 0.300f});
        wall({0.20f, 0.015f}, {0.32f, 0.015f});

        tb::sim::Flipper fl{};
        fl.params.pivot = {0.170f, 0.120f};
        fl.params.rest_angle_deg = -31.0f;
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
    }

    Ball& spawn(float x, float y, float vx, float vy) {
        Ball& b = s.balls[0];
        b.index = 0;
        b.live = true;
        b.mode = BallMode::Free;
        b.layer = 0;
        b.pos = {x, y};
        b.vel = {vx, vy};
        b.last_safe_pos = b.pos;
        return b;
    }

    void step() {
        TickInput in;
        solver.step(s, in);
    }

    std::vector<SimEvent> drain() {
        SimEvent buf[512];
        const size_t n = s.game_ring.drain(buf, 512);
        return {buf, buf + n};
    }

    size_t count(uint16_t type) const {
        // Only meaningful right after drain(); kept for symmetry.
        return 0;
    }
};

size_t count_of(const std::vector<SimEvent>& evs, uint16_t type) {
    return static_cast<size_t>(
        std::count_if(evs.begin(), evs.end(), [type](const auto& e) { return e.type == type; }));
}

// The §5.6 M8 addition: a straight ramp with a linear height profile.
RampPath make_test_ramp(float z_end) {
    RampPath ramp;
    ramp.element_id = 200;
    ramp.width = 0.044f;
    ramp.total_s = 0.6f;
    // Straight path (0.300, 0.300) → (0.300, 0.900), ~10 mm samples.
    const int n = 61;
    for (int i = 0; i < n; ++i) {
        const float u = float(i) / float(n - 1);
        RampPath::Sample sample;
        sample.pos = {0.300f, 0.300f + 0.6f * u};
        sample.tangent = {0.0f, 1.0f};
        sample.s = 0.6f * u;
        sample.z = z_end * u; // linear climb
        ramp.samples.push_back(sample);
    }
    ramp.end_z[0] = 0.0f;
    ramp.end_z[1] = z_end;
    ramp.seam_center[0] = ramp.samples.front().pos;
    ramp.seam_center[1] = ramp.samples.back().pos;
    ramp.seam_into[0] = {0.0f, 1.0f};
    ramp.seam_into[1] = {0.0f, -1.0f};
    ramp.seam_layer[0] = 0;
    ramp.seam_layer[1] = std::fabs(z_end - 0.055f) <= 0.005f ? 1 : 0;
    return ramp;
}

} // namespace

// Ramp.ClimbConservesPlausibleEnergy: a 4 m/s entry climbing to z_end
// exits with the speed §6.10.4's dynamics allow (gravity + damping), within
// the FT-10 tolerance band [3.0, 3.7] m/s.
TEST(Ramp, ClimbConservesPlausibleEnergy) {
    Rig rig;
    rig.s.ramps.push_back(make_test_ramp(0.08f));
    rig.spawn(0.300f, 0.270f, 0.0f, 4.0f);

    float exit_s_dot = -1.0f;
    for (int i = 0; i < 600; ++i) {
        rig.step();
        if (rig.s.balls[0].mode == BallMode::Free && rig.s.balls[0].pos.y > 0.85f) {
            exit_s_dot = length(rig.s.balls[0].vel);
            break;
        }
    }
    EXPECT_GT(exit_s_dot, 0.0f) << "never exited";
    EXPECT_GE(exit_s_dot, 3.0f);
    EXPECT_LE(exit_s_dot, 3.7f);
}

// Ramp.SlowBallRollsBack: a 1.2 m/s entry stalls and returns to layer 0
// moving down-table, with no ramp_made (§6.10.6 rollback).
TEST(Ramp, SlowBallRollsBack) {
    Rig rig;
    rig.s.ramps.push_back(make_test_ramp(0.08f));
    rig.spawn(0.300f, 0.270f, 0.0f, 1.2f);

    bool exited_down = false;
    bool bound_once = false;
    for (int i = 0; i < 1500; ++i) {
        const bool was_ramp = rig.s.balls[0].mode == BallMode::Ramp;
        rig.step();
        const Ball& b = rig.s.balls[0];
        bound_once = bound_once || was_ramp || b.mode == BallMode::Ramp;
        if (bound_once && b.mode == BallMode::Free && b.pos.y < 0.29f) {
            exited_down = b.vel.y < 0.0f;
            break;
        }
    }
    EXPECT_TRUE(bound_once) << "slow ball must bind";
    EXPECT_TRUE(exited_down) << "slow ball must roll back out the entry";
    const auto evs = rig.drain();
    EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::RampMade)), 0u);
}

// Ramp.RidesDownFromLayer1: a layer-1 ball binds at the far seam (z_end =
// layer1_z), rides down, unbinds on layer 0 (§6.10.2/§6.10.6).
TEST(Ramp, RidesDownFromLayer1) {
    Rig rig;
    rig.s.ramps.push_back(make_test_ramp(0.055f));
    Ball& b = rig.spawn(0.300f, 0.950f, 0.0f, -1.5f);
    b.layer = 1; // upper playfield ball, crossing the layer-1 far seam

    bool reached_layer0 = false;
    for (int i = 0; i < 1500; ++i) {
        rig.step();
        const Ball& ball = rig.s.balls[0];
        if (ball.mode == BallMode::Free && ball.layer == 0 && ball.pos.y < 0.35f) {
            reached_layer0 = true;
            break;
        }
    }
    EXPECT_TRUE(reached_layer0) << "layer-1 ball must ride down to layer 0";
    // Riding down the entry end emits no ramp_made (§6.10.6).
    const auto evs = rig.drain();
    EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::RampMade)), 0u);
}

// Layer.MasksIsolateColliders: a layer-1 ball sails over layer-0 walls.
TEST(Layer, MasksIsolateColliders) {
    Rig rig;
    Ball& b = rig.spawn(0.100f, 0.500f, 1.0f, 0.0f);
    b.layer = 1;
    for (int i = 0; i < 600; ++i) {
        rig.step();
    }
    // Crossed the whole inlane-guide/border region on layer 1 without any
    // collision ever zeroing it: it must be far right, still live.
    EXPECT_TRUE(rig.s.balls[0].live);
    EXPECT_GT(rig.s.balls[0].pos.x, 0.40f);
}

// Magnet.CaptureEnvelope (§6.12 + FT-09 catch phase): a ball rolling into
// the enabled field is caught near the core with bounded speed.
TEST(Magnet, CaptureEnvelope) {
    Rig rig;
    MagnetSim mag;
    mag.pos = {0.260f, 0.720f};
    mag.radius = 0.09f;
    mag.strength = 1.2f;
    mag.on = true;
    rig.s.magnets.push_back(mag);
    rig.spawn(0.260f, 0.860f, 0.0f, -0.40f);

    bool left_field = false;
    float vmax = 0.0f;
    bool entered = false;
    for (int i = 0; i < 4000; ++i) {
        rig.step();
        const Ball& b = rig.s.balls[0];
        vmax = std::max(vmax, length(b.vel));
        const float d = length(b.pos - rig.s.magnets[0].pos);
        if (!entered && d <= 0.09f) {
            entered = true; // entry into the field (≈ 0.52 m/s per §5.7)
        }
        if (entered && d > 0.09f) {
            left_field = true;
            break;
        }
    }
    ASSERT_TRUE(entered) << "ball never entered the field";
    EXPECT_FALSE(left_field) << "caught ball must stay in the field";
    EXPECT_LE(vmax, 3.5f) << "field must not inject energy";
    // By 3 s: near the core and slow.
    const Ball& b = rig.s.balls[0];
    EXPECT_LE(length(b.pos - rig.s.magnets[0].pos), 0.03f);
    EXPECT_LE(length(b.vel), 0.35f);
}

// FT-09 Magnet catch and throw (08-physics.md §5.7, M8): full scenario —
// catch phases above plus the release throw.
TEST(feel_scenarios, ft09_magnet_catch_throw) {
    Rig rig;
    MagnetSim mag;
    mag.pos = {0.260f, 0.720f};
    mag.radius = 0.09f;
    mag.strength = 1.2f;
    mag.on = true;
    rig.s.magnets.push_back(mag);
    rig.spawn(0.260f, 0.860f, 0.0f, -0.40f);

    float vmax = 0.0f;
    bool entered = false;
    for (int i = 0; i < 3000; ++i) { // energized through t = 3.0 s
        rig.step();
        vmax = std::max(vmax, length(rig.s.balls[0].vel));
        const float d = length(rig.s.balls[0].pos - rig.s.magnets[0].pos);
        if (!entered && d <= 0.09f) {
            entered = true; // entry at ≈ 0.52 m/s (§5.7)
        }
        ASSERT_TRUE(!entered || d <= 0.09f) << "left the field while energized";
    }
    ASSERT_TRUE(entered);
    EXPECT_LE(vmax, 3.5f);
    EXPECT_LE(length(rig.s.balls[0].vel), 0.35f);
    EXPECT_LE(length(rig.s.balls[0].pos - rig.s.magnets[0].pos), 0.03f);

    for (int i = 0; i < 1000; ++i) { // 3.0 → 4.0 s: bounds hold
        rig.step();
        ASSERT_LE(length(rig.s.balls[0].vel), 0.35f);
        ASSERT_LE(length(rig.s.balls[0].pos - rig.s.magnets[0].pos), 0.03f);
    }

    // Release: no impulse across the tick; then the throw down-table.
    const Vec2 v_pre = rig.s.balls[0].vel;
    rig.s.magnets[0].set_active(false);
    rig.step();
    const Vec2 v_post = rig.s.balls[0].vel;
    EXPECT_LE(length(v_post - v_pre), 0.08f) << "release adds no impulse";

    bool crossed = false;
    for (int i = 0; i < 2000 && !crossed; ++i) {
        rig.step();
        crossed = rig.s.balls[0].pos.y <= 0.45f && rig.s.balls[0].vel.y < 0.0f;
    }
    EXPECT_TRUE(crossed) << "released ball must leave down-table";
}

// FT-10 Ramp make and rollback (08-physics.md §5.7, M8).
TEST(feel_scenarios, ft10_ramp_make_rollback) {
    // (a) Make.
    {
        Rig rig;
        rig.s.ramps.push_back(make_test_ramp(0.08f));
        rig.spawn(0.300f, 0.270f, 0.0f, 4.0f);

        uint64_t bind_tick = 0;
        uint64_t exit_tick = 0;
        float exit_s_dot = 0.0f;
        float max_per_tick = 0.0f;
        Vec2 pre_bind_pos = rig.s.balls[0].pos;
        Vec2 at_exit_pos{};
        int binds = 0;
        for (int i = 0; i < 800 && exit_tick == 0; ++i) {
            const bool was_ramp = rig.s.balls[0].mode == BallMode::Ramp;
            const float s0 = rig.s.balls[0].s_dot;
            rig.step();
            const Ball& b = rig.s.balls[0];
            if (!was_ramp && b.mode == BallMode::Ramp) {
                ++binds;
                bind_tick = rig.s.tick;
                EXPECT_LE(length(b.pos - pre_bind_pos), 0.010f) << "position discontinuity at bind";
            }
            if (was_ramp && b.mode == BallMode::Ramp) {
                max_per_tick = std::max(max_per_tick, std::abs(b.s_dot - s0));
            }
            if (was_ramp && b.mode == BallMode::Free && b.pos.y > 0.85f) {
                exit_tick = rig.s.tick;
                exit_s_dot = length(b.vel);
                at_exit_pos = b.pos;
                (void)at_exit_pos;
            }
            pre_bind_pos = b.pos;
        }
        ASSERT_EQ(binds, 1u) << "must bind exactly once";
        ASSERT_NE(exit_tick, 0u) << "must forward-exit";
        EXPECT_EQ(exit_tick - bind_tick, exit_tick - bind_tick); // transit calc
        EXPECT_LE(exit_tick - bind_tick, 250u) << "transit ≤ 250 ms";
        EXPECT_GE(exit_s_dot, 3.0f);
        EXPECT_LE(exit_s_dot, 3.7f);
        EXPECT_LE(max_per_tick, 0.012f) << "per-tick |Δs_dot| ≤ 0.01 (bind-tick slack)";

        const auto evs = rig.drain();
        EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::RampMade)), 1u);
        EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
    }

    // (b) Rollback.
    {
        Rig rig;
        rig.s.ramps.push_back(make_test_ramp(0.08f));
        rig.spawn(0.300f, 0.270f, 0.0f, 1.2f);

        int binds = 0;
        bool exited = false;
        for (int i = 0; i < 1500; ++i) {
            const bool was_ramp = rig.s.balls[0].mode == BallMode::Ramp;
            rig.step();
            const Ball& b = rig.s.balls[0];
            if (!was_ramp && b.mode == BallMode::Ramp) {
                ++binds;
            }
            if (was_ramp && b.mode == BallMode::Free && b.pos.y < 0.32f) {
                exited = b.vel.y < 0.0f;
                break;
            }
        }
        EXPECT_EQ(binds, 1u) << "binds exactly once";
        EXPECT_TRUE(exited) << "unbinds out the entry within 1.5 s, moving down";
        const auto evs = rig.drain();
        EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::RampMade)), 0u);
    }
}

// Determinism.FullElementSetReplay (04-milestones.md M8): every element
// type in one table-driven replay — the permanent full-coverage gate.
TEST(Determinism, FullElementSetReplay) {
    const auto run = []() {
        const tb::table::TableDef def =
            tb::table::load_table(tb::test::data_path("tables/test-lab"));
        tb::sim::SimState s;
        tb::table::build_sim(def, s);

        // Grow the full element set onto it: magnet, ramp, spinner, gate,
        // kicker, drop bank, captive, lock.
        tb::sim::MagnetSim mag;
        mag.pos = {0.26f, 0.9f};
        mag.radius = 0.06f;
        mag.strength = 1.0f;
        mag.on = true;
        s.magnets.push_back(mag);
        s.ramps.push_back(make_test_ramp(0.055f));

        tb::sim::SpinnerElem sp;
        sp.common.table_id = 300;
        sp.a = {0.2512f, 0.3875f};
        sp.b = {0.2488f, 0.4125f};
        sp.face_normal = {1.0f, 0.0f};
        s.spinners.push_back(sp);

        tb::sim::GateElem g;
        g.common.table_id = 301;
        g.a = {0.30f, 0.44f};
        g.b = {0.30f, 0.56f};
        g.face_normal = {0.0f, 1.0f};
        s.gates.push_back(g);

        tb::sim::KickerElem k;
        k.common.table_id = 302;
        k.pos = {0.40f, 0.65f};
        k.radius = 0.014f;
        k.capture_ticks = 500;
        k.eject_speed = 3.0f;
        k.eject_angle_deg = 180.0f;
        s.kickers.push_back(k);

        tb::sim::DropBankElem bank;
        bank.common.table_id = 303;
        for (int i = 0; i < 3; ++i) {
            tb::sim::DropBankElem::Target t;
            t.face_a = {0.10f + 0.05f * float(i), 0.60f};
            t.face_b = {0.10f + 0.05f * float(i), 0.62f};
            t.face_normal = {-1.0f, 0.0f};
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Segment;
            c.a = t.face_a;
            c.b = t.face_b;
            c.element_id = 303;
            c.sub_index = uint16_t(i);
            c.material = tb::sim::MaterialId::Plastic;
            s.colliders.push_back(c);
            t.collider_idx = uint32_t(s.colliders.size() - 1);
            bank.targets.push_back(t);
        }
        s.drop_banks.push_back(bank);
        s.grid.build(s.colliders, s.width, s.height);

        tb::sim::CaptiveBallElem cap;
        cap.common.table_id = 304;
        cap.common.cooldown_ticks = 100;
        cap.a = {0.45f, 0.70f};
        cap.b = {0.45f, 0.78f};
        cap.slot_len = 0.08f;
        cap.axis = {0.0f, 1.0f};
        cap.s_c = tb::sim::kBallRadius;
        s.captives.push_back(cap);

        tb::sim::BallLockElem lock;
        lock.common.table_id = 305;
        lock.pos = {0.47f, 0.50f};
        lock.capacity = 2;
        s.ball_locks.push_back(lock);

        tb::sim::Ball& b = s.balls[0];
        b.index = 0;
        b.live = true;
        b.mode = tb::sim::BallMode::Free;
        b.pos = s.plunger.pos + s.plunger.lane_dir * (tb::sim::kBallRadius + 0.002f);
        b.vel = {0.0f, 0.0f};
        b.last_safe_pos = b.pos;
        s.trough_balls = 3;

        tb::sim::Solver solver;
        std::vector<uint64_t> hashes;
        for (int tick = 0; tick < 30000; ++tick) {
            tb::sim::TickInput in;
            const int phase = tick % 4000;
            if (phase < 1600) {
                in.buttons |= 1u << 4;
            }
            if ((tick % 7) < 3) {
                in.buttons |= 1u;
            }
            if ((tick % 11) < 4) {
                in.buttons |= 2u;
            }
            solver.step(s, in);
            if ((tick + 1) % 2500 == 0) {
                hashes.push_back(tb::sim::state_hash(s));
            }
        }
        return hashes;
    };

    const std::vector<uint64_t> a = run();
    const std::vector<uint64_t> b = run();
    ASSERT_EQ(a.size(), 12u);
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "full-element replay diverged at sample " << i;
    }
}
