#include "sim/elements.h"

#include "sim/solver.h"
#include "support/data_path.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

// M6 element sims (04-milestones.md §M6; 08-physics.md §6.2–§6.8).
namespace {

using tb::sim::Ball;
using tb::sim::BallMode;
using tb::sim::Collider;
using tb::sim::SimState;
using tb::sim::Solver;
using tb::sim::TickInput;
using tb::sim::Vec2;

struct MiniRig {
    SimState s;
    Solver solver;
    uint16_t base_element = 100; // element ids for baked colliders

    MiniRig() {
        s.slope_deg = 0.0f; // flat: isolated element kinematics
        s.width = 0.52f;
        s.height = 1.04f;
    }

    uint16_t add_border() {
        Collider w{};
        w.kind = Collider::Kind::Segment;
        w.element_id = base_element++;
        w.material = tb::sim::MaterialId::Wood;
        w.a = {0, 0};
        w.b = {0.52f, 0};
        s.colliders.push_back(w);
        return w.element_id;
    }

    void finish() { s.grid.build(s.colliders, s.width, s.height); }

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

    // Drains all game-ring events.
    std::vector<tb::sim::SimEvent> drain_events() {
        tb::sim::SimEvent buf[256];
        const size_t n = s.game_ring.drain(buf, 256);
        return {buf, buf + n};
    }

    void step(uint32_t buttons = 0) {
        TickInput in;
        in.buttons = buttons;
        solver.step(s, in);
    }
};

size_t count_events(const std::vector<tb::sim::SimEvent>& evs, uint16_t type) {
    return static_cast<size_t>(
        std::count_if(evs.begin(), evs.end(), [type](const auto& e) { return e.type == type; }));
}

} // namespace

// Slingshot.FiresOnBandCrossing: a fast ball onto the face gets the kick
// formula (tangential kept, normal forced ≥ kick_speed); a slow ball below
// the 0.4 m/s band does not (§6.2).
TEST(Slingshot, FiresOnBandCrossing) {
    for (float impact : {2.0f, 0.3f}) {
        MiniRig rig;
        tb::sim::SlingshotElem sl;
        sl.common.table_id = 7;
        sl.common.cooldown_ticks = 80;
        sl.face_a = {0.2f, 0.2f};
        sl.face_b = {0.2f, 0.3f};
        sl.face_normal = {-1.0f, 0.0f}; // left of a→b: fires −x (toward the ball)
        sl.kick_speed = 3.5f;
        rig.s.slingshots.push_back(sl);

        Collider face{};
        face.kind = Collider::Kind::Segment;
        face.a = sl.face_a;
        face.b = sl.face_b;
        face.element_id = 7;
        face.material = tb::sim::MaterialId::Rubber;
        rig.s.colliders.push_back(face);
        rig.finish();

        rig.spawn(0.15f, 0.25f, impact, 0.0f);
        for (int i = 0; i < (impact > 1.0f ? 60 : 250); ++i) {
            rig.step();
        }
        const auto evs = rig.drain_events();
        if (impact >= 0.4f) {
            EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
            // Kick fires along the face normal (away from the sling):
            // outgoing −x speed at least kick_speed.
            EXPECT_LE(rig.s.balls[0].vel.x, -3.4f);
        } else {
            EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 0u)
                << "dead-zone contact must not fire";
        }
    }
}

// PopBumper.KickVectorRadial: any touch off cooldown kicks along the
// jittered radial; the outgoing speed grows by up to kick_speed (§6.3).
TEST(PopBumper, KickVectorRadial) {
    MiniRig rig;
    tb::sim::PopBumperElem pop;
    pop.common.table_id = 9;
    pop.common.cooldown_ticks = 60;
    pop.pos = {0.26f, 0.5f};
    pop.radius = 0.031f;
    pop.kick_speed = 4.5f;
    rig.s.pop_bumpers.push_back(pop);

    Collider circle{};
    circle.kind = Collider::Kind::Point;
    circle.a = pop.pos;
    circle.radius = pop.radius;
    circle.element_id = 9;
    circle.material = tb::sim::MaterialId::Rubber;
    rig.s.colliders.push_back(circle);
    rig.finish();

    rig.spawn(0.26f, 0.42f, 0.0f, 1.0f); // straight up into the cap
    for (int i = 0; i < 40; ++i) {
        rig.step();
    }
    const auto evs = rig.drain_events();
    EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
    // Radial kick pushes the ball back down (jitter ≤ 0.12 rad keeps it
    // mostly -y); speed grew by roughly kick_speed.
    EXPECT_LT(rig.s.balls[0].vel.y, 0.0f);
    EXPECT_NEAR(length(rig.s.balls[0].vel), 4.5f, 1.5f);
}

// Standup.EmitsSwitchHitOnce: one switch per hit, debounced by the 100 ms
// cooldown; back-side contacts never trigger (§6.4).
TEST(Standup, EmitsSwitchHitOnce) {
    MiniRig rig;
    tb::sim::StandupTargetElem st;
    st.common.table_id = 11;
    st.common.cooldown_ticks = 100;
    st.face_a = {0.25f, 0.45f};
    st.face_b = {0.25f, 0.55f};
    st.face_normal = {-1.0f, 0.0f}; // faces the approaching ball (§6.4)
    st.min_speed = 0.3f;
    rig.s.standups.push_back(st);

    Collider face{};
    face.kind = Collider::Kind::Segment;
    face.a = st.face_a;
    face.b = st.face_b;
    face.element_id = 11;
    face.material = tb::sim::MaterialId::Plastic;
    rig.s.colliders.push_back(face);
    rig.finish();

    rig.spawn(0.15f, 0.5f, 2.0f, 0.0f);
    for (int i = 0; i < 200; ++i) {
        rig.step();
    }
    const auto evs = rig.drain_events();
    // Exactly one event: the strike plus any rattle is inside the 100 ms
    // debounce window.
    EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
}

// Gate.OneWayBlocksReverse: forward passes (with a switch), reverse
// bounces off absorbent steel (§6.7).
TEST(Gate, OneWayBlocksReverse) {
    for (int direction = 0; direction < 2; ++direction) {
        MiniRig rig;
        tb::sim::GateElem g;
        g.common.table_id = 13;
        g.a = {0.26f, 0.45f};
        g.b = {0.26f, 0.55f};
        g.face_normal = {0.0f, 1.0f}; // forward = +y
        g.state = tb::sim::GateState::OneWay;
        g.mechanical = true;
        rig.s.gates.push_back(g);
        rig.finish();

        if (direction == 0) {
            rig.spawn(0.26f, 0.4f, 0.0f, 2.0f); // forward: passes
            for (int i = 0; i < 150; ++i) {
                rig.step();
            }
            EXPECT_GT(rig.s.balls[0].pos.y, 0.53f) << "forward ball must clear the gate";
            const auto evs = rig.drain_events();
            EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
        } else {
            rig.spawn(0.26f, 0.6f, 0.0f, -2.0f); // reverse: blocked
            for (int i = 0; i < 150; ++i) {
                rig.step();
            }
            EXPECT_GT(rig.s.balls[0].pos.y, 0.55f) << "reverse ball must be blocked";
            const auto evs = rig.drain_events();
            EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 0u)
                << "a blocked ball emits no switch";
        }
    }
}

// Rollover.TriggersAtOverlap: center entering the 0.012 m capsule fires the
// switch_hit + rollover pair once; staying inside does not re-fire (§6.8).
TEST(Rollover, TriggersAtOverlap) {
    MiniRig rig;
    tb::sim::RolloverElem ro;
    ro.common.table_id = 15;
    ro.a = {0.25f, 0.48f};
    ro.b = {0.25f, 0.52f};
    ro.armed = true;
    rig.s.rollovers.push_back(ro);
    rig.finish();

    rig.spawn(0.15f, 0.5f, 0.5f, 0.0f); // slow pass through the capsule
    for (int i = 0; i < 200; ++i) {
        rig.step();
    }
    const auto evs = rig.drain_events();
    EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 1u);
    EXPECT_EQ(count_events(evs, uint16_t(tb::sim::SimEventType::RolloverEvent)), 1u);
}

// Spinner.SpinCountFromBallSpeed: a 4 m/s crossing spins the plate to
// ~100 rad/s; revolutions emit switch_hit + spinner_spin pairs with rpm
// derived from the instantaneous ω (§6.6). The ball loses 0.12 m/s along
// the pass direction.
TEST(Spinner, SpinCountFromBallSpeed) {
    MiniRig rig;
    tb::sim::SpinnerElem sp;
    sp.common.table_id = 17;
    sp.a = {0.2512f, 0.4875f};
    sp.b = {0.2488f, 0.5125f}; // 0.025 m span, ⊥ facing (+x)
    sp.face_normal = {1.0f, 0.0f};
    rig.s.spinners.push_back(sp);
    rig.finish();

    Ball& b = rig.spawn(0.10f, 0.5f, 4.0f, 0.0f);
    // Step exactly through the crossing tick.
    for (int i = 0; i < 40; ++i) {
        rig.step();
    }
    EXPECT_GT(std::abs(rig.s.spinners[0].plate_omega), 50.0f)
        << "4 m/s crossing must spin the plate (~100 rad/s)";

    // Ball slowed by the plate inertia: ≤ 4 − 0.12 + noise, still moving.
    EXPECT_LT(b.vel.x, 4.0f);
    EXPECT_GT(b.vel.x, 3.5f);

    // Let the plate run: revolutions emit event pairs.
    for (int i = 0; i < 3000; ++i) {
        rig.step();
    }
    const auto evs = rig.drain_events();
    const size_t spins = count_events(evs, uint16_t(tb::sim::SimEventType::SpinnerSpin));
    const size_t switches = count_events(evs, uint16_t(tb::sim::SimEventType::SwitchHit));
    EXPECT_GE(spins, 1u);
    EXPECT_EQ(spins, switches) << "each revolution pairs switch_hit + spinner_spin";
}

// Determinism.TestLabAllElementsReplay (04-milestones.md M6): a driven
// 30 s run over the loaded test-lab (pops, targets, rollover, slings,
// gates from the shooter prefab, flippers) hashes identically twice.
TEST(Determinism, TestLabAllElementsReplay) {
    const auto run = []() {
        const tb::table::TableDef def =
            tb::table::load_table(tb::test::data_path("tables/test-lab"));
        tb::sim::SimState s;
        tb::table::build_sim(def, s);
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
                in.buttons |= 1u << 4; // plunge
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
        ASSERT_EQ(a[i], b[i]) << "test-lab replay diverged at sample " << i;
    }
}

// Builder regression (review cycle 1): a table-authored spinner bakes its
// trigger segment perpendicular to facing_deg, and table rollovers start
// armed.
TEST(Spinner, BuilderBakesPerpendicularSegment) {
    tb::table::TableDef def;
    def.width = 0.52f;
    def.height = 1.04f;
    tb::table::SpinnerDef sp_def;
    sp_def.id = "sp";
    sp_def.pos[0] = 0.26f;
    sp_def.pos[1] = 0.5f;
    sp_def.facing_deg = 90.0f;
    def.elements.push_back(tb::table::Element{sp_def});
    tb::table::RolloverDef ro_def;
    ro_def.id = "ro";
    ro_def.pos[0] = 0.2f;
    ro_def.pos[1] = 0.5f;
    def.elements.push_back(tb::table::Element{ro_def});

    tb::sim::SimState sim;
    tb::table::build_sim(def, sim);
    ASSERT_EQ(sim.spinners.size(), 1u);
    const auto& sp = sim.spinners[0];
    // facing 90° = +y: the segment must be horizontal (⊥ to +y).
    EXPECT_NEAR(sp.a.y, sp.b.y, 1e-6f);
    EXPECT_NEAR(sp.face_normal.x, 0.0f, 1e-6f);
    EXPECT_NEAR(sp.face_normal.y, 1.0f, 1e-6f);
    EXPECT_NEAR(length(sp.b - sp.a), 0.025f, 1e-5f);
    ASSERT_EQ(sim.rollovers.size(), 1u);
    EXPECT_TRUE(sim.rollovers[0].armed);
}
