#include "sim/script_host.h"
#include "sim/solver.h"
#include "support/data_path.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

// M9 scripting suites (04-milestones.md §M9; 10-scripting.md).
namespace {

using tb::sim::Ball;
using tb::sim::BallMode;
using tb::sim::ScriptHost;
using tb::sim::SimEvent;
using tb::sim::SimEventType;
using tb::sim::SimState;
using tb::sim::Solver;
using tb::sim::TickInput;

// A minimal rig with an element id table so script ids resolve. Fills
// the caller's state (SimState is non-copyable, non-movable).
void make_rig(SimState& s) {
    s.slope_deg = 6.5f;
    s.width = 0.52f;
    s.height = 1.04f;
    s.has_plunger = true;
    s.plunger.pos = {0.5f, 0.03f};
    s.plunger.lane_dir = {0.0f, 1.0f};

    s.element_ids = {"pop_main",
                     "light_pop",
                     "light_top_lane",
                     "gear_bank",
                     "pit_scoop",
                     "drift_magnet",
                     "loop_left_switch",
                     "flippers_left_flipper",
                     "shooter_gate"};
    s.element_tags = {{"scoring"}, {}, {}, {}, {}, {}, {}, {}, {}};

    tb::sim::LightState light;
    light.table_id = 1;
    s.lights.push_back(light);
    light.table_id = 2;
    s.lights.push_back(light);

    tb::sim::MagnetSim mag;
    mag.table_id = 5;
    mag.pos = {0.26f, 0.72f};
    s.magnets.push_back(mag);

    tb::sim::GateElem gate;
    gate.common.table_id = 8;
    gate.a = {0.26f, 0.4f};
    gate.b = {0.26f, 0.5f};
    gate.face_normal = {0.0f, 1.0f};
    gate.mechanical = true;
    s.gates.push_back(gate);

    s.grid.build(s.colliders, s.width, s.height);
}

// Load helper: SimState is non-copyable (atomic rings), so each test
// builds its own rig inline via make_rig() + host.load().
struct Loaded {
    std::unique_ptr<SimState> state;
    ScriptHost host;

    void load(const std::string& rules) {
        state = std::make_unique<SimState>();
        make_rig(*state);
        state->script = &host;
        host.load(rules, *state);
    }
};

SimEvent
synth(SimEventType type, uint16_t element, float a = 0.0f, uint8_t ball = 0, float b = 0.0f) {
    SimEvent ev;
    ev.type = uint16_t(type);
    ev.element = element;
    ev.a = a;
    ev.b = b;
    ev.data = ball;
    return ev;
}

} // namespace

// Sandbox.IoOsRequireLoadAreNil (§1.2): the escape libraries are nil and
// math.random raises.
TEST(Sandbox, IoOsRequireLoadAreNil) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.state.io_ok = io == nil
        tb.state.os_ok = os == nil
        tb.state.require_ok = require == nil
        tb.state.load_ok = load == nil
        tb.state.loadfile_ok = loadfile == nil
        tb.state.string_dump_ok = string.dump == nil
        tb.state.math_random_raised = false
        local ok, err = pcall(math.random)
        if not ok and string.find(err, "tb.rng") then
          tb.state.math_random_raised = true
        end
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_tick(1);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(1);

    int64_t v = 0;
    ASSERT_TRUE(host.state_read_int(1, "io_ok", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "os_ok", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "require_ok", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "load_ok", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "string_dump_ok", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "math_random_raised", v));
    EXPECT_EQ(v, 1);
}

// Sandbox.WatchdogKillsRunawayHandler (§2.4): an infinite loop handler is
// aborted by the budget, permanently disabled, and the sim continues.
TEST(Sandbox, WatchdogKillsRunawayHandler) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.on("switch_hit", function(ev)
          if ev.id == "pop_main" then
            while true do end   -- runaway: must be killed
          end
        end)
        tb.on("switch_hit", function(ev)
          tb.score(10)          -- later handler runs on later ticks
        end)
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_game(1);

    host.begin_tick(1);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(1);
    // The runaway burned the budget and was disabled mid-tick; the second
    // handler was skipped this tick (§2.4).
    EXPECT_EQ(host.player_scores(1).score, 0u);
    EXPECT_LT(host.watchdog_budget_remaining(), 10000);

    // Budget refills next tick; the runaway stays disabled; the second
    // handler works.
    host.begin_tick(2);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(2);
    EXPECT_EQ(host.player_scores(1).score, 10u);
    EXPECT_EQ(host.watchdog_budget_remaining(), 10000);
}

// Api.EveryCanonNameExists (PLAN.md §5.7): both lists — events accepted by
// tb.on and callable tb.* names — with exactly the four tables.
TEST(Api, EveryCanonNameExists) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        local events = {
          "game_start", "ball_start", "ball_end", "game_end", "player_up",
          "ball_launched", "switch_hit", "target_down", "bank_complete",
          "spinner_spin", "rollover", "kicker_enter", "ramp_made", "drain",
          "ball_save_expired", "tilt_warning", "tilt", "ball_lock",
          "captive_full_travel", "multiball_start", "multiball_end",
          "timer_tick",
        }
        for _, name in ipairs(events) do
          tb.on(name, function() end)   -- raises on unknown
        end
        local functions = {
          "score", "add_bonus", "set_multiplier", "award_extra_ball",
          "light_on", "light_off", "light_blink", "play_sound", "play_music",
          "stop_music", "kick", "kick_hold", "release_lock", "magnet_on",
          "magnet_off", "magnet_pulse", "set_flipper_enabled", "timer",
          "cancel_timer", "ball_save", "add_ball", "drop_bank_reset",
          "gate_open", "gate_close", "show_message", "rng", "rng_range",
        }
        tb.state.bad = 0
        for _, name in ipairs(functions) do
          if type(tb[name]) ~= "function" then tb.state.bad = tb.state.bad + 1 end
        end
        local tables = { "backglass", "state", "game", "table_info" }
        for _, name in ipairs(tables) do
          if type(tb[name]) ~= "table" then tb.state.bad = tb.state.bad + 1 end
        end
    )lua"));

    ScriptHost& host = loaded.host;
    int64_t v = 0;
    ASSERT_TRUE(host.state_read_int(1, "bad", v));
    EXPECT_EQ(v, 0);
}

// Events.PayloadGoldenPerType: each dispatched event type arrives with the
// §4 payload fields, echoed through tb.state.
TEST(Events, PayloadGoldenPerType) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.on("switch_hit", function(ev)
          tb.state.sw_id = ev.id
          tb.state.sw_ball = ev.ball_id
          tb.state.sw_speed = ev.speed
          tb.state.sw_tag1 = ev.tags[1]
        end)
        tb.on("target_down", function(ev)
          tb.state.td_bank = ev.bank_id
          tb.state.td_idx = ev.target_index
        end)
        tb.on("spinner_spin", function(ev)
          tb.state.sp_rpm = ev.rpm
        end)
        tb.on("ball_lock", function(ev)
          tb.state.bl_lock = ev.lock_id
          tb.state.bl_count = ev.count
        end)
        tb.on("drain", function(ev)
          tb.state.dr_remaining = ev.balls_remaining
        end)
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_game(1);

    host.begin_tick(1);
    SimEvent sw = synth(SimEventType::SwitchHit, 0, 2.0f, /*ball=*/0);
    host.dispatch(sw);
    host.end_tick(1);

    int64_t v = 0;
    std::string sv;
    ASSERT_TRUE(host.state_read_int(1, "sw_ball", v));
    EXPECT_EQ(v, 1); // ball index 0 → 1-based id 1
    ASSERT_TRUE(host.state_read_int(1, "sw_speed", v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(host.state_read_string(1, "sw_tag1", sv)); // tags propagate
    EXPECT_EQ(sv, "scoring");

    host.begin_tick(2);
    host.dispatch(synth(SimEventType::TargetDown, 3, 0.0f, 0, /*target_index=*/2.0f));
    host.end_tick(2);
    ASSERT_TRUE(host.state_read_int(1, "td_idx", v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(host.state_read_string(1, "td_bank", sv)); // bank_id = element id
    EXPECT_EQ(sv, "gear_bank");

    host.begin_tick(3);
    host.dispatch(synth(SimEventType::SpinnerSpin, 6, 120.0f));
    host.end_tick(3);
    ASSERT_TRUE(host.state_read_int(1, "sp_rpm", v));
    EXPECT_EQ(v, 120);

    host.begin_tick(4);
    host.dispatch(synth(SimEventType::BallLockCapture, 4, /*count=*/2.0f));
    host.end_tick(4);
    ASSERT_TRUE(host.state_read_int(1, "bl_count", v));
    EXPECT_EQ(v, 2);

    host.begin_tick(5);
    host.dispatch(synth(SimEventType::Drain, 0xFFFF, /*remaining=*/3.0f));
    host.end_tick(5);
    ASSERT_TRUE(host.state_read_int(1, "dr_remaining", v));
    EXPECT_EQ(v, 3);
}

// Determinism.ScriptRngReplayStable (§2.6): rules using tb.rng replay
// bit-identically — same seed, same event stream, same draws.
TEST(Determinism, ScriptRngReplayStable) {
    const auto run = [](uint64_t seed) {
        Loaded loaded;
        loaded.state = std::make_unique<SimState>();
        make_rig(*loaded.state);
        loaded.state->rng_script.seed(seed, 2);
        loaded.state->script = &loaded.host;
        loaded.host.load(R"lua(
            tb.on("switch_hit", function(ev)
              if ev.speed > 1.0 then
                tb.score(tb.rng_range(100, 999))
              else
                tb.score(math.floor(tb.rng() * 50))
              end
            end)
        )lua",
                         *loaded.state);
        loaded.host.begin_game(1);
        loaded.state->rng_script.seed(seed, 2); // reset post-load draws
        Solver solver;
        for (int t = 0; t < 500; ++t) {
            TickInput in;
            solver.step(*loaded.state, in);
            if (t % 7 == 0) {
                loaded.state->script->begin_tick(loaded.state->tick);
                SimEvent ev = synth(SimEventType::SwitchHit, 0, t % 14 == 0 ? 2.0f : 0.5f);
                loaded.state->script->dispatch(ev);
                loaded.state->script->end_tick(loaded.state->tick);
            }
        }
        return std::pair<uint64_t, uint64_t>(tb::sim::state_hash(*loaded.state),
                                             loaded.host.player_scores(1).score);
    };

    const auto a = run(1234);
    const auto b = run(1234);
    EXPECT_EQ(a.first, b.first);
    EXPECT_EQ(a.second, b.second);
    EXPECT_GT(a.second, 0u);
}

// NeonDrift.ScriptedGameReachesGameEnd: the real table + real rules, a
// canned-input 3-ball game driven through the framework seam; score > 0
// and game_end observed.
TEST(NeonDrift, ScriptedGameReachesGameEnd) {
    const tb::table::TableDef def = tb::table::load_table(tb::test::data_path("tables/neon-drift"));
    tb::sim::SimState s;
    tb::table::build_sim(def, s);
    ASSERT_TRUE(s.has_plunger);

    ScriptHost host;
    s.script = &host;
    std::ifstream rules(tb::test::data_path("tables/neon-drift/rules.lua"));
    ASSERT_TRUE(rules.good());
    std::stringstream buf;
    buf << rules.rdbuf();
    ASSERT_NO_THROW(host.load(buf.str(), s));

    host.begin_game(1);

    // Framework seam stand-in: a 3-ball game. ball_start per ball,
    // ball_end + next ball on drain, game_end after ball 3.
    Solver solver;
    int ball = 1;
    int drains_left = 3;
    bool game_end_seen = false;
    int64_t drains_seen = 0;

    // Ball 1 starts before the loop: the drain branch below is live from
    // t == 0, so a late-armed initial ball_start would mis-sequence events
    // if launch timing ever changed (review cycle 4).
    host.fire_event("ball_start", {{"player", 1}, {"ball_number", 1}});

    for (int t = 0; t < 240000 && !game_end_seen; ++t) {
        TickInput in;
        const int phase = t % 5000;
        if (phase < 1600) {
            in.buttons |= 1u << 4; // plunger: charge + release cycles
        }
        if ((t % 9) < 4) {
            in.buttons |= 1u; // flipper mash feeds slings/orbit/ramps
        }
        if ((t % 13) < 5) {
            in.buttons |= 2u;
        }
        solver.step(s, in);

        // Drains surface through the rules (they count them in tb.state);
        // the solver itself dispatches tick events to Lua inside step().
        int64_t drains = 0;
        if (host.state_read_int(1, "drains", drains) && drains > drains_seen) {
            drains_seen = drains;
            host.fire_event("ball_end",
                            {{"player", 1},
                             {"ball_number", ball},
                             {"bonus", int64_t(host.player_scores(1).bonus)},
                             {"bonus_multiplier", host.player_scores(1).bonus_multiplier}});
            --drains_left;
            if (drains_left > 0) {
                ++ball;
                host.fire_event("ball_start", {{"player", 1}, {"ball_number", ball}});
            } else {
                host.end_game();
                game_end_seen = true;
            }
        }
    }

    EXPECT_TRUE(game_end_seen) << "game never reached game_end";
    EXPECT_GT(host.player_scores(1).score, 0u);
}

// Watchdog bypass regression 1 (review cycle 1): a catchable-error loop
// (`while true do pcall(tight loop) end`) must not outlive the budget.
TEST(Sandbox, WatchdogKillsPcallLoop) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.on("switch_hit", function(ev)
          while true do pcall(function() while true do end end) end
        end)
        tb.on("switch_hit", function(ev) tb.score(10) end)
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_game(1);
    host.begin_tick(1);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(1);
    // Returns (no hang) and the budget was drained.
    EXPECT_LT(host.watchdog_budget_remaining(), 10000);

    // Next tick: budget refilled, second handler runs, first stays dead.
    host.begin_tick(2);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(2);
    EXPECT_EQ(host.player_scores(1).score, 10u);
}

// Watchdog bypass regression 2: an infinite coroutine.wrap loop.
TEST(Sandbox, WatchdogKillsCoroutineWrapLoop) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.on("switch_hit", function(ev)
          local w = coroutine.wrap(function() while true do end end)
          while true do w() end
        end)
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_game(1);
    host.begin_tick(1);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(1);
    EXPECT_LT(host.watchdog_budget_remaining(), 10000);
}

// tb.rng_range bounds (BLOCKER 2): out-of-int-range values raise a Lua
// error instead of reaching UB conversions.
TEST(Api, RngRangeRejectsOutOfBounds) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.state.ok = 0
        tb.state.err1 = false
        tb.state.err2 = false
        tb.state.err3 = false
        if pcall(tb.rng_range, 0, 5e9) then tb.state.ok = tb.state.ok + 1
        else tb.state.err1 = true end
        if pcall(tb.rng_range, 0, 1e19) then tb.state.ok = tb.state.ok + 1
        else tb.state.err2 = true end
        if pcall(tb.rng_range, -1e300, 1e300) then tb.state.ok = tb.state.ok + 1
        else tb.state.err3 = true end
    )lua"));

    ScriptHost& host = loaded.host;
    host.begin_game(1);
    host.begin_tick(1);
    host.dispatch(synth(SimEventType::SwitchHit, 0));
    host.end_tick(1);

    int64_t v = 0;
    ASSERT_TRUE(host.state_read_int(1, "err1", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "err2", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "err3", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(host.state_read_int(1, "ok", v));
    EXPECT_EQ(v, 0);
}

// tb.on rejects unknown event names (§2.3) — negative test.
TEST(Api, OnRejectsUnknownEventName) {
    Loaded loaded;
    EXPECT_THROW(loaded.load(R"lua(
        tb.on("definitely_not_an_event", function() end)
    )lua"),
                 std::runtime_error);
}

// Drain payload off-by-one (review major): one ball draining alone
// reports balls_remaining == 0.
TEST(Events, DrainPayloadExcludesDrainingBall) {
    Loaded loaded;
    ASSERT_NO_THROW(loaded.load(R"lua(
        tb.on("drain", function(ev) tb.state.remaining = ev.balls_remaining end)
    )lua"));
    ScriptHost& host = loaded.host;
    host.begin_game(1);

    // Force a drain through the real region logic: ball into the outhole.
    tb::sim::SimState& s = *loaded.state;
    s.outholes.push_back({{0.02f, 0.01f}, {0.46f, 0.01f}});
    Ball& ball = s.balls[0]; // fixed array Ball[kMaxBalls] — always valid
    ball.index = 0;
    ball.live = true;
    ball.mode = BallMode::Free;
    ball.layer = 0;
    ball.pos = {0.2f, 0.012f}; // inside the outhole capsule
    ball.vel = {0.0f, 0.0f};
    ball.last_safe_pos = ball.pos;

    tb::sim::Solver solver;
    tb::sim::TickInput in;
    solver.step(s, in);

    int64_t v = 0;
    ASSERT_TRUE(host.state_read_int(1, "remaining", v));
    EXPECT_EQ(v, 0);
}
