#include "core/rng.h"
#include "sim/elements.h"
#include "sim/solver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

// M7 element sims (04-milestones.md §M7; 08-physics.md §6.5, §6.9,
// §6.13–§6.15).
namespace {

using tb::sim::Ball;
using tb::sim::BallMode;
using tb::sim::Collider;
using tb::sim::SimEvent;
using tb::sim::SimState;
using tb::sim::Solver;
using tb::sim::TickInput;
using tb::sim::Vec2;

struct Rig {
    SimState s;
    Solver solver;

    Rig() {
        s.slope_deg = 0.0f; // flat: isolated element kinematics
        s.width = 0.52f;
        s.height = 1.04f;
        s.ball_count = 4;
        s.trough_balls = 0;
    }

    void finish() { s.grid.build(s.colliders, s.width, s.height); }

    Ball& spawn(uint8_t idx, float x, float y, float vx, float vy) {
        Ball& b = s.balls[idx];
        b.index = idx;
        b.live = true;
        b.mode = BallMode::Free;
        b.layer = 0;
        b.pos = {x, y};
        b.vel = {vx, vy};
        b.last_safe_pos = b.pos;
        return b;
    }

    std::vector<SimEvent> drain() {
        SimEvent buf[512];
        const size_t n = s.game_ring.drain(buf, 512);
        return {buf, buf + n};
    }

    void step() {
        TickInput in;
        solver.step(s, in);
    }

    size_t count(uint16_t type, uint16_t element = 0xFFFF) const {
        return 0; // drained counts only; helper kept for readability
    }
};

size_t count_of(const std::vector<SimEvent>& evs, uint16_t type) {
    return static_cast<size_t>(
        std::count_if(evs.begin(), evs.end(), [type](const auto& e) { return e.type == type; }));
}

} // namespace

// Kicker.CaptureDwellEject: capture at any speed (scoop), hold for
// capture_ms, eject along eject_angle_deg at eject_speed (§6.9).
TEST(Kicker, CaptureDwellEject) {
    Rig rig;
    tb::sim::KickerElem k;
    k.common.table_id = 21;
    k.pos = {0.26f, 0.5f};
    k.radius = 0.014f;
    k.capture_ticks = 300; // 300 ms dwell
    k.eject_speed = 3.0f;
    k.eject_angle_deg = 90.0f;
    k.style = tb::sim::KickerStyle::Scoop;
    rig.s.kickers.push_back(k);
    rig.finish();

    rig.spawn(0, 0.26f, 0.5f, 0.0f, 0.0f); // settled in the zone
    rig.step();
    EXPECT_EQ(rig.s.balls[0].mode, BallMode::Captured);
    EXPECT_EQ(rig.s.kickers[0].held_ball, 0);

    // Dwelling: no eject before the failsafe (the capture tick already
    // decremented the countdown to 299).
    for (int i = 0; i < 298; ++i) {
        rig.step();
    }
    EXPECT_EQ(rig.s.balls[0].mode, BallMode::Captured);

    // The countdown reaching 0 ejects along +y at eject_speed.
    rig.step();
    EXPECT_EQ(rig.s.balls[0].mode, BallMode::Free);
    EXPECT_NEAR(rig.s.balls[0].vel.x, 0.0f, 1e-4f);
    EXPECT_NEAR(rig.s.balls[0].vel.y, 3.0f, 0.2f);
    EXPECT_EQ(rig.s.kickers[0].held_ball, 0xFF);
}

// DropBank.CompletesAndResets: three facing-side hits drop three targets
// (switch_hit + target_down each), the last completing the bank; a script
// reset raises all (§6.5).
TEST(DropBank, CompletesAndResets) {
    Rig rig;
    tb::sim::DropBankElem bank;
    bank.common.table_id = 23;
    for (int i = 0; i < 3; ++i) {
        const float x = 0.15f + 0.05f * float(i);
        tb::sim::DropBankElem::Target t;
        t.face_a = {x, 0.44f};
        t.face_b = {x, 0.46f};
        t.face_normal = {-1.0f, 0.0f}; // faces the approaching ball (−x)
        tb::sim::Collider c{};
        c.kind = Collider::Kind::Segment;
        c.a = t.face_a;
        c.b = t.face_b;
        c.element_id = 23;
        c.sub_index = uint16_t(i);
        c.material = tb::sim::MaterialId::Plastic;
        rig.s.colliders.push_back(c);
        t.collider_idx = uint32_t(rig.s.colliders.size() - 1);
        bank.targets.push_back(t);
    }
    rig.s.drop_banks.push_back(bank);
    rig.finish();

    // Strike each face in sequence from the left, fast enough (≥ 0.3 m/s).
    const float xs[3] = {0.15f, 0.20f, 0.25f};
    for (int which = 0; which < 3; ++which) {
        rig.spawn(0, xs[which] - 0.05f, 0.45f, 2.0f, 0.0f);
        for (int i = 0; i < 400 && rig.s.drop_banks[0].targets[size_t(which)].state ==
                                       tb::sim::DropTargetState::Up;
             ++i) {
            rig.step();
        }
        EXPECT_EQ(rig.s.drop_banks[0].targets[size_t(which)].state,
                  tb::sim::DropTargetState::Dropping)
            << "target " << which << " did not drop";
    }

    const auto evs = rig.drain();
    EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::TargetDown)), 3u);
    EXPECT_EQ(count_of(evs, uint16_t(tb::sim::SimEventType::SwitchHit)), 3u);

    // Let the drop animations finish: bank completes once all DOWN.
    for (int i = 0; i < 200; ++i) {
        rig.step();
    }
    auto evs2 = rig.drain();
    EXPECT_EQ(count_of(evs2, uint16_t(tb::sim::SimEventType::BankComplete)), 1u);

    // Script reset (§6.5): all targets raise and come back UP.
    for (auto& t : rig.s.drop_banks[0].targets) {
        t.state = tb::sim::DropTargetState::Raising;
        t.anim_ticks = 250;
    }
    for (int i = 0; i < 260; ++i) {
        rig.step();
    }
    for (const auto& t : rig.s.drop_banks[0].targets) {
        EXPECT_EQ(t.state, tb::sim::DropTargetState::Up);
    }
}

// CaptiveBall.StaysInLane: 10,000 ticks of pounding never leave the slot
// (§6.13 clamp invariant).
TEST(CaptiveBall, StaysInLane) {
    Rig rig;
    tb::sim::CaptiveBallElem cap;
    cap.common.table_id = 25;
    cap.common.cooldown_ticks = 100;
    cap.a = {0.1f, 0.3f};
    cap.b = {0.1f, 0.4f};
    cap.slot_len = 0.1f;
    cap.axis = {0.0f, 1.0f};
    cap.s_c = tb::sim::kBallRadius;
    rig.s.captives.push_back(cap);
    rig.finish();

    // Pound with a fast ball from below repeatedly.
    for (int round = 0; round < 40; ++round) {
        rig.spawn(0, 0.1f, 0.2f, 0.0f, 3.0f);
        for (int i = 0; i < 250; ++i) {
            rig.step();
            const tb::sim::CaptiveBallElem& c = rig.s.captives[0];
            EXPECT_GE(c.s_c, tb::sim::kBallRadius - 1e-6f);
            EXPECT_LE(c.s_c, c.slot_len - tb::sim::kBallRadius + 1e-6f);
        }
    }
}

// CaptiveBall.FullTravelEmitsOnceAfterSwitchHit: a hard strike drives the
// captive to the far end at ≥ 0.3 m/s → switch_hit then exactly one
// captive_full_travel on a later tick; a weak strike emits only the
// switch_hit (§6.13).
TEST(CaptiveBall, FullTravelEmitsOnceAfterSwitchHit) {
    // The arrival-speed gate needs gravity decelerating the climb: the rig
    // runs at the default slope with the slot pointing up-table.
    for (float speed : {3.0f, 0.5f}) {
        Rig rig;
        rig.s.slope_deg = 6.5f;
        tb::sim::CaptiveBallElem cap;
        cap.common.table_id = 27;
        cap.common.cooldown_ticks = 100;
        cap.a = {0.1f, 0.3f};
        cap.b = {0.1f, 0.4f};
        cap.slot_len = 0.1f;
        cap.axis = {0.0f, 1.0f};
        cap.s_c = tb::sim::kBallRadius;
        rig.s.captives.push_back(cap);
        rig.finish();

        // Spawn close enough that the weakest strike still reaches the
        // captive against slope + rolling resistance.
        rig.spawn(0, 0.1f, 0.25f, 0.0f, speed);
        for (int i = 0; i < 800; ++i) {
            rig.step();
        }
        const auto evs = rig.drain();
        const size_t switches = count_of(evs, uint16_t(tb::sim::SimEventType::SwitchHit));
        const size_t travels = count_of(evs, uint16_t(tb::sim::SimEventType::CaptiveFullTravel));
        EXPECT_GE(switches, 1u) << "the strike must emit switch_hit";
        if (speed > 2.0f) {
            EXPECT_EQ(travels, 1u) << "hard strike: exactly one full travel";
            // Ordering: the strike's switch precedes the travel.
            size_t sw_tick = 0;
            size_t tr_tick = 0;
            for (const auto& e : evs) {
                if (e.type == uint16_t(tb::sim::SimEventType::SwitchHit) && sw_tick == 0) {
                    sw_tick = size_t(e.tick);
                }
                if (e.type == uint16_t(tb::sim::SimEventType::CaptiveFullTravel)) {
                    tr_tick = size_t(e.tick);
                }
            }
            EXPECT_LT(sw_tick, tr_tick);
        } else {
            // Weak strike: arrival below 0.3 m/s bounces silently.
            EXPECT_EQ(travels, 0u) << "weak strike must not emit full travel";
        }
    }
}

// Trough.CountsNeverGoNegative: property test — random serve/drain
// sequences keep active + trough + locked == ball_count (04 §M7).
TEST(Trough, CountsNeverGoNegative) {
    Rig rig;
    rig.s.has_plunger = true;
    rig.s.plunger.pos = {0.5f, 0.03f};
    rig.s.plunger.lane_dir = {0.0f, 1.0f};
    rig.s.ball_count = 4;
    rig.s.trough_balls = 4;
    rig.finish();

    tb::Pcg32 rng;
    rng.seed(1234, 5678);
    auto count_active = [&]() {
        int n = 0;
        for (const Ball& b : rig.s.balls) {
            if (b.live && b.mode == BallMode::Free) {
                ++n;
            }
        }
        return n;
    };

    for (int op = 0; op < 2000; ++op) {
        const uint32_t roll = rng.next_float() * 100.0f;
        if (roll < 40) {
            // Serve: emulate the M5 loop's serve step.
            if (rig.s.trough_balls > 0 && count_active() == 0) {
                --rig.s.trough_balls;
                uint8_t next_idx = 0;
                for (Ball& b : rig.s.balls) {
                    const bool slot_free = !b.live;
                    const uint8_t idx = next_idx;
                    ++next_idx;
                    if (slot_free) {
                        b.index = idx;
                        b.live = true;
                        b.mode = BallMode::Free;
                        b.pos = rig.s.plunger.pos +
                                rig.s.plunger.lane_dir * (tb::sim::kBallRadius + 0.002f);
                        b.vel = {0.0f, 0.0f};
                        break;
                    }
                }
            }
        } else if (roll < 80) {
            // Drain a random live ball.
            for (Ball& b : rig.s.balls) {
                if (b.live && b.mode == BallMode::Free) {
                    b.live = false;
                    ++rig.s.trough_balls;
                    break;
                }
            }
        }
        rig.step();
        EXPECT_GE(rig.s.trough_balls, 0);
        EXPECT_LE(rig.s.trough_balls, rig.s.ball_count);
        const int total = count_active() + rig.s.trough_balls + rig.s.locked_balls;
        EXPECT_EQ(total, rig.s.ball_count)
            << "op " << op << ": active(" << count_active() << ") + trough(" << rig.s.trough_balls
            << ") + locked(" << rig.s.locked_balls << ")";
    }
}

// BallSave.TimerServesWithinWindow: a drain inside the window re-serves
// on the plunger without consuming the ball (M7 mechanism).
TEST(BallSave, TimerServesWithinWindow) {
    Rig rig;
    rig.s.has_plunger = true;
    rig.s.plunger.pos = {0.5f, 0.03f};
    rig.s.plunger.lane_dir = {0.0f, 1.0f};
    rig.s.ball_count = 4;
    rig.s.trough_balls = 3;
    rig.s.outholes.push_back({{0.1f, 0.01f}, {0.4f, 0.01f}});
    rig.finish();

    rig.spawn(0, 0.2f, 0.01f, 0.0f, 0.0f); // sitting in the drain region
    rig.s.ball_save.active = true;
    rig.s.ball_save.ticks_left = 5000;

    rig.step();
    // Saved: still live, back on the plunger, window consumed.
    EXPECT_TRUE(rig.s.balls[0].live);
    EXPECT_EQ(rig.s.balls[0].mode, BallMode::Free);
    EXPECT_NEAR(rig.s.balls[0].pos.y, rig.s.plunger.pos.y + tb::sim::kBallRadius + 0.002f, 1e-4f);
    EXPECT_FALSE(rig.s.ball_save.active);
    EXPECT_EQ(rig.s.trough_balls, 3); // no trough change

    // Window expiry: the countdown reaches zero and deactivates.
    rig.s.ball_save.active = true;
    rig.s.ball_save.ticks_left = 10;
    for (int i = 0; i < 10; ++i) {
        rig.step();
    }
    EXPECT_FALSE(rig.s.ball_save.active);
}

// MultiBall.ThreeActiveDeterministic: three balls juggled on a small
// walled box hash identically across two in-process runs.
TEST(MultiBall, ThreeActiveDeterministic) {
    const auto run = []() {
        Rig rig;
        // A closed box so balls stay in play.
        const tb::sim::Vec2 box[5] = {
            {0.1f, 0.2f}, {0.4f, 0.2f}, {0.4f, 0.6f}, {0.1f, 0.6f}, {0.1f, 0.2f}};
        for (int i = 0; i < 4; ++i) {
            Collider w{};
            w.kind = Collider::Kind::Segment;
            w.a = box[i];
            w.b = box[i + 1];
            w.element_id = 90 + i;
            w.material = tb::sim::MaterialId::Wood;
            rig.s.colliders.push_back(w);
        }
        rig.finish();
        rig.s.rng_sim.seed(4242, 7);
        rig.spawn(0, 0.2f, 0.3f, 1.5f, 0.5f);
        rig.spawn(1, 0.3f, 0.5f, -0.9f, 1.2f);
        rig.spawn(2, 0.25f, 0.4f, 0.3f, -1.6f);
        for (int i = 0; i < 20000; ++i) {
            rig.step();
        }
        return tb::sim::state_hash(rig.s);
    };
    const uint64_t a = run();
    const uint64_t b = run();
    EXPECT_EQ(a, b);
}
