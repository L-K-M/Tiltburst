// M10 game-framework tests (04-milestones.md §M10; 11-game-framework.md).
// The rig mirrors script_test.cpp's: a minimal SimState + loaded host,
// with the FSM attached exactly as the app attaches it.
#include "core/rng.h"
#include "core/time.h"
#include "game/game_machine.h"
#include "game/high_scores.h"
#include "game/score_format.h"
#include "sim/script_host.h"
#include "sim/solver.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

int temp_dir_counter = 0;

// RAII temp dir: removes itself even when an ASSERT returns early.
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& prefix)
        : path(std::filesystem::temp_directory_path() /
               (prefix + "_" + std::to_string(tb_now_ns()) + "_" +
                std::to_string(++temp_dir_counter))) {
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

using namespace tb;
using tb::game::GameMachine;
using tb::game::GameState;
using tb::game::HighScoreTable;

struct Rig {
    sim::ScriptHost host;
    std::unique_ptr<sim::SimState> state;
    sim::Solver solver;

    void load(const std::string& rules) {
        state = std::make_unique<sim::SimState>();
        make_rig(*state);
        state->script = &host;
        // "Framework attached" from construction (solver.h: fsm_step !=
        // nullptr is the single source): a no-op phase-3 stub keeps the
        // M5 auto-serve loop out of these tests exactly as in
        // production; each test's attach() replaces it with the real
        // machine before the first step.
        state->fsm_step = [](void*, sim::SimState&, const sim::TickInput&) {};
        host.load(rules, *state);
    }

    // One solver tick with the given button state; the FSM runs in
    // phase 3 via the same hook the app installs.
    template <typename F>
    void step(F&& attach, uint32_t buttons) {
        // Runs every step: attach() must be IDEMPOTENT — re-point
        // fsm_ctx/fsm_step at the persistent machine, never rebuild it
        // (a construct-per-call lambda would wipe FSM state per tick).
        attach();
        solver.step(*state, sim::TickInput{buttons});
    }

    static void make_rig(sim::SimState& s) {
        s.slope_deg = 6.5f;
        s.width = 0.52f;
        s.height = 1.04f;
        s.has_plunger = true;
        s.plunger.pos = {0.5f, 0.03f};
        s.plunger.lane_dir = {0.0f, 1.0f};
        s.trough_balls = 4;

        s.element_ids = {"pop_main"};
        s.element_tags = {{"scoring"}};
        s.outholes.push_back({{0.02f, 0.01f}, {0.46f, 0.01f}});
        s.grid.build(s.colliders, s.width, s.height);
    }
};

// Minimal rules that record lifecycle events into tb.state for
// assertions.
const char* kRules = R"lua(
    tb.on("game_start", function(ev) tb.state.gs_players = ev.player_count end)
    tb.on("ball_start", function(ev)
        tb.state.bs = (tb.state.bs or 0) + 1
        tb.state.last_player = ev.player
        tb.state.last_ball = ev.ball_number
    end)
    tb.on("ball_end", function(ev)
        tb.state.be = (tb.state.be or 0) + 1
        tb.state.be_bonus = ev.bonus
        tb.state.be_mult = ev.bonus_multiplier
    end)
    tb.on("player_up", function(ev)
        tb.state.pu = (tb.state.pu or 0) + 1
        tb.state.last_up = ev.player
        tb.state.last_prev = ev.previous_player
    end)
    tb.on("tilt_warning", function(ev) tb.state.tw = ev.count end)
    tb.on("tilt", function() tb.state.tilted = true end)
    tb.on("ball_save_expired", function() tb.state.save_expired = true end)
    tb.on("multiball_start", function(ev) tb.state.mb = ev.ball_count end)
    tb.on("multiball_end", function() tb.state.mb_end = true end)
    tb.on("game_end", function(ev) tb.state.winner = ev.winner end)
    tb.on("switch_hit", function(ev) tb.score(1000) end)
)lua";

// Launch: move live balls out of the plunger lane and deliver the
// BallLaunched transition through the machine's injection seam (the real
// event comes from the plunger inside a tick; between ticks is
// equivalent for the state transition).
void launch_ball(Rig& rig, GameMachine& m) {
    for (auto& b : rig.state->balls) {
        if (b.live && b.mode == sim::BallMode::Free) {
            b.pos = {0.25f, 0.4f};
            b.vel = {0.0f, 0.0f};
        }
    }
    sim::SimEvent launched;
    launched.type = uint16_t(sim::SimEventType::BallLaunched);
    m.inject_sim_event(launched);
}

// Nudge burst: `kicks` same-direction presses 10 ticks apart at level
// 3, then `gap_ticks` of decay so the bob re-arms (08 §7.2: re-arm at
// 0.7x threshold; exp(-1.35 t) decay). Three level-3 kicks peak
// |p| ~ 3*0.35/9 = 0.117 m, sweeping both bob thresholds in one burst
// (warn then hard, threshold order); alternating-direction pounding
// would saturate the abuse accumulator without re-arming it.
void nudge_burst(Rig& rig, const GameMachine& m, uint32_t nudge_bit, int kicks, int gap_ticks) {
    for (int k = 0; k < kicks; ++k) {
        rig.solver.step(*rig.state, sim::TickInput{nudge_bit});
        for (int t = 0; t < 10; ++t) {
            rig.solver.step(*rig.state, sim::TickInput{0});
        }
    }
    (void)m;
    for (int t = 0; t < gap_ticks; ++t) {
        rig.solver.step(*rig.state, sim::TickInput{0});
    }
}

// Wait out the default 8 s ball save so a drain ends the ball (the
// save is single-use; after expiry the drain is final).
void wait_out_ball_save(Rig& rig, const GameMachine& m) {
    for (int t = 0; t < 8'100; ++t) {
        rig.solver.step(*rig.state, sim::TickInput{0});
    }
    (void)m;
}

game::FrameworkConfig base_cfg() {
    game::FrameworkConfig cfg;
    cfg.date_stamp = "2026-08-27";
    return cfg;
}

} // namespace

// ---- TransitionTableGolden: legal transitions reachable; unlisted
// (state, event) pairs ignored (04 §M10 tests list) ----
TEST(GameMachine, TransitionTableGolden) {
    Rig rig;
    ASSERT_NO_THROW(rig.load(kRules));
    HighScoreTable scores;
    GameMachine m(rig.host, *rig.state, scores, base_cfg());

    auto attach = [&m, &rig] {
        rig.state->fsm_ctx = &m;
        rig.state->fsm_step = [](void* ctx, sim::SimState& s, const sim::TickInput& in) {
            static_cast<GameMachine*>(ctx)->step(in);
        };
    };

    // T1 happened in the constructor: Boot → Attract.
    EXPECT_EQ(m.state(), GameState::Attract);

    // Unlisted input in Attract (flipper mash, plunger, pause) must be
    // ignored: stay in Attract for many ticks.
    for (int t = 0; t < 500; ++t) {
        rig.step(attach, /*buttons=*/(1u << 0) | (1u << 1) | (1u << 4) | (1u << 9));
    }
    EXPECT_EQ(m.state(), GameState::Attract);

    // T3: Attract --Start--> TableSelect; T6 back on plunger.
    rig.step(attach, 1u << 8);
    EXPECT_EQ(m.state(), GameState::TableSelect);
    rig.step(attach, 0);
    rig.step(attach, 1u << 4);
    EXPECT_EQ(m.state(), GameState::Attract);

    // T3 → T5: Start starts the game.
    rig.step(attach, 0);
    rig.step(attach, 1u << 8); // Attract → TableSelect
    rig.step(attach, 0);
    rig.step(attach, 1u << 8); // TableSelect → GameStarting
    EXPECT_EQ(m.state(), GameState::GameStarting);

    // Start presses in GameStarting while P1 is on ball 1 add players
    // (§3.1): 3 more presses → 4 players.
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    EXPECT_EQ(m.player_count(), 4);
    // A 5th Start does nothing (cap 4).
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    EXPECT_EQ(m.player_count(), 4);

    // T8: 2000-tick intro → serve → BallReady, and ball_start fired
    // with P1 ball 1.
    for (int t = 0; t < 2'100 && m.state() != GameState::BallReady; ++t) {
        rig.step(attach, 0);
    }
    EXPECT_EQ(m.state(), GameState::BallReady);
    int64_t v = 0;
    ASSERT_TRUE(rig.host.state_read_int(1, "bs", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(rig.host.state_read_int(1, "last_player", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(rig.host.state_read_int(1, "last_ball", v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(rig.host.state_read_int(1, "gs_players", v));
    EXPECT_EQ(v, 1); // game_start fired at entry: 1 player; the 3 joins
                     // after it update tb.game.player_count live
}

// ---- Players.FourPlayerRotationOrder: P1..P4 alternate per ball,
// player_up fires, scores per player ----
TEST(Players, FourPlayerRotationOrder) {
    Rig rig;
    ASSERT_NO_THROW(rig.load(kRules));
    HighScoreTable scores;
    GameMachine m(rig.host, *rig.state, scores, base_cfg());
    auto attach = [&m, &rig] {
        rig.state->fsm_ctx = &m;
        rig.state->fsm_step = [](void* ctx, sim::SimState& s, const sim::TickInput& in) {
            static_cast<GameMachine*>(ctx)->step(in);
        };
    };

    // Start the game and add to 4 players (Start presses during P1B1).
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    for (int i = 0; i < 3; ++i) {
        rig.step(attach, 1u << 8);
        rig.step(attach, 0);
    }
    ASSERT_EQ(m.player_count(), 4);

    // Drive full balls: reach BallInPlay (launch when BallReady), let
    // the default save lapse, park every live ball in the outhole, and
    // wait out BonusCount/PlayerChange/HighScoreEntry (initials entry
    // auto-commits its 3 Start presses per qualifying player — all four
    // zero-score players qualify against an empty list, §7).
    std::vector<int> rotation;
    int prev_player = 1;
    bool game_over = false;
    for (int ball_i = 0; ball_i < 40 && !game_over; ++ball_i) {
        for (int t = 0; t < 100'000; ++t) {
            if (m.state() == GameState::BallReady) {
                launch_ball(rig, m);
            } else if (m.state() == GameState::HighScoreEntry) {
                // Start confirms a slot (3 per player) — alternate for
                // a rising edge each press.
                rig.solver.step(*rig.state, sim::TickInput{1u << 8});
                rig.solver.step(*rig.state, sim::TickInput{0});
                continue;
            }
            if (m.state() == GameState::BallInPlay || m.state() == GameState::GameOver) {
                break;
            }
            rig.solver.step(*rig.state, sim::TickInput{0});
        }
        if (m.state() == GameState::GameOver) {
            game_over = true;
            break;
        }
        ASSERT_EQ(m.state(), GameState::BallInPlay) << "ball " << ball_i;
        if (m.current_player() != prev_player) {
            rotation.push_back(m.current_player());
            prev_player = m.current_player();
        }

        wait_out_ball_save(rig, m);
        // Park every live ball in the outhole; hold it there until the
        // drain registers.
        for (int t = 0; t < 100'000 && m.state() == GameState::BallInPlay; ++t) {
            for (auto& b : rig.state->balls) {
                if (b.live && b.mode == sim::BallMode::Free) {
                    b.pos = {0.2f, 0.012f};
                    b.vel = {0.0f, 0.0f};
                }
            }
            rig.solver.step(*rig.state, sim::TickInput{0});
        }
    }
    EXPECT_TRUE(game_over);

    // 12 balls, 4 players: the incoming-player sequence after P1's
    // first ball is 2,3,4,1,2,3,4,1,2,3,4 (11 changes).
    ASSERT_EQ(rotation.size(), 11u);
    const int expected[11] = {2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4};
    for (size_t i = 0; i < 11; ++i) {
        EXPECT_EQ(rotation[i], expected[i]) << "rotation step " << i;
    }

    // player_up fired for every incoming player: each player's OWN
    // tb.state table saw pu >= 1 with last_up == their number (the
    // swap contract, §3.2).
    for (int p = 2; p <= 4; ++p) {
        int64_t pu = 0, last_up = 0;
        ASSERT_TRUE(rig.host.state_read_int(p, "pu", pu));
        EXPECT_GE(pu, 1) << "player " << p;
        ASSERT_TRUE(rig.host.state_read_int(p, "last_up", last_up));
        EXPECT_EQ(last_up, p) << "player " << p;
    }
    // game_end fired with a winner (kRules records it into the table
    // of whoever was current at GameOver — the last player).
    int64_t winner = 0;
    ASSERT_TRUE(rig.host.state_read_int(m.current_player(), "winner", winner));
    EXPECT_GE(winner, 1);
    EXPECT_LE(winner, 4);
}

// ---- Tilt.WarningsThenTiltAtThreshold: 2 warnings then tilt on the
// 3rd danger event; danger comes from the §7.2 bob ----
TEST(Tilt, WarningsThenTiltAtThreshold) {
    Rig rig;
    ASSERT_NO_THROW(rig.load(kRules));
    HighScoreTable scores;
    game::FrameworkConfig cfg = base_cfg();
    cfg.tilt_warnings = 2;
    GameMachine m(rig.host, *rig.state, scores, cfg);
    auto attach = [&m, &rig] {
        rig.state->fsm_ctx = &m;
        rig.state->fsm_step = [](void* ctx, sim::SimState& s, const sim::TickInput& in) {
            static_cast<GameMachine*>(ctx)->step(in);
        };
    };

    // Get into BallInPlay.
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    for (int t = 0; t < 2'100 && m.state() != GameState::BallReady; ++t) {
        rig.step(attach, 0);
    }
    ASSERT_EQ(m.state(), GameState::BallReady);
    // Launch.
    launch_ball(rig, m);
    rig.step(attach, 0);
    ASSERT_EQ(m.state(), GameState::BallInPlay);

    // §7.2: a 2-kick level-3 burst peaks |p| ~ 2*0.35/9 = 0.078 — over
    // warn (0.055), under hard (0.085); the pair only pumps abuse to
    // 0.7 < 1.2, so each burst is exactly ONE warn crossing. Bursts 1
    // and 2 are the two warnings; burst 3's crossing is TILT.
    // 1500-tick gaps re-arm warn (peak decays to ~0.030 < 0.0385).
    rig.state->nudge_level = 3;
    const uint32_t nudge_left = 1u << 5;
    for (int w = 0; w < 2; ++w) {
        for (int burst = 0; burst < 4 && m.tilt_warning_count() < w + 1; ++burst) {
            nudge_burst(rig, m, nudge_left, 2, 1'500);
        }
        ASSERT_EQ(m.tilt_warning_count(), w + 1) << "warning " << (w + 1);
        EXPECT_FALSE(m.tilted());
    }
    for (int burst = 0; burst < 4 && !m.tilted(); ++burst) {
        nudge_burst(rig, m, nudge_left, 2, 1'500);
    }
    EXPECT_TRUE(m.tilted());
    EXPECT_EQ(m.tilt_warning_count(), 3); // the tilt event is the 3rd
    int64_t tw = 0;
    ASSERT_TRUE(rig.host.state_read_int(1, "tw", tw));
    EXPECT_EQ(tw, 2); // exactly two warnings before the tilt
    int64_t tilted = 0;
    ASSERT_TRUE(rig.host.state_read_int(1, "tilted", tilted));
    EXPECT_EQ(tilted, 1);

    // §7.3: after the tilted ball drains, the next ball starts with
    // warnings + danger state reset.
    for (auto& b : rig.state->balls) {
        if (b.live) {
            b.pos = {0.2f, 0.012f};
            b.vel = {0.0f, 0.0f};
        }
    }
    for (int t = 0; t < 400'000 && m.state() == GameState::BallInPlay; ++t) {
        rig.step(attach, 0);
    }
    for (int t = 0;
         t < 400'000 && m.state() != GameState::BallInPlay && m.state() != GameState::GameOver;
         ++t) {
        rig.step(attach, 0);
    }
    EXPECT_FALSE(m.tilted());
    EXPECT_EQ(m.tilt_warning_count(), 0);
}

// ---- Tilt.FlippersDeadAfterTilt ----
TEST(Tilt, FlippersDeadAfterTilt) {
    Rig rig;
    ASSERT_NO_THROW(rig.load(kRules));
    sim::Flipper left;
    left.enabled = true;
    left.params.action = 0;
    rig.state->flippers.push_back(left);

    HighScoreTable scores;
    game::FrameworkConfig cfg = base_cfg();
    cfg.tilt_warnings = 1;
    GameMachine m(rig.host, *rig.state, scores, cfg);
    auto attach = [&m, &rig] {
        rig.state->fsm_ctx = &m;
        rig.state->fsm_step = [](void* ctx, sim::SimState& s, const sim::TickInput& in) {
            static_cast<GameMachine*>(ctx)->step(in);
        };
    };

    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    for (int t = 0; t < 2'100 && m.state() != GameState::BallReady; ++t) {
        rig.step(attach, 0);
    }
    launch_ball(rig, m);
    rig.step(attach, 0);
    ASSERT_EQ(m.state(), GameState::BallInPlay);

    EXPECT_TRUE(rig.state->flippers_enabled);
    EXPECT_TRUE(rig.state->coils_enabled);

    // Nudge until tilt (warnings = 1: burst 1's warn+hard pair is
    // warning then TILT).
    rig.state->nudge_level = 3;
    const uint32_t nudge = 1u << 5;
    for (int burst = 0; burst < 6 && !m.tilted(); ++burst) {
        nudge_burst(rig, m, nudge, 3, 1'500);
    }
    ASSERT_TRUE(m.tilted());

    // The latched FlippersEnabled(false)/CoilsEnabled(false) actions
    // drain on the NEXT tick's phase 1.
    rig.step(attach, 0);
    EXPECT_FALSE(rig.state->flippers_enabled);
    EXPECT_FALSE(rig.state->coils_enabled);

    // A held flipper button no longer actuates the bat (§5): angle stays
    // at rest while pressed.
    const float theta_rest = rig.state->flippers[0].theta;
    for (int t = 0; t < 200; ++t) {
        rig.step(attach, 1u << 0);
    }
    EXPECT_EQ(rig.state->flippers[0].theta, theta_rest);
}

// ---- HighScores.PersistRoundTripAndOrdering ----
TEST(HighScores, PersistRoundTripAndOrdering) {
    HighScoreTable t;
    TempDir dir("tb_scores_test");
    const auto path = dir.path / "test-table.json";

    game::HighScoreEntry e;
    e.date = "2026-08-27";
    e.score = 100;
    e.initials = {'Z', 'Z', 'Z'};
    EXPECT_EQ(t.insert(e), 1);
    e.score = 300;
    e.initials = {'A', 'A', 'A'};
    EXPECT_EQ(t.insert(e), 1);
    e.score = 200;
    e.initials = {'M', 'M', 'M'};
    EXPECT_EQ(t.insert(e), 2); // sorted below the 300
    // Ties insert BELOW equal scores (§7).
    e.score = 200;
    e.initials = {'N', 'N', 'N'};
    EXPECT_EQ(t.insert(e), 3);

    t.save(path, "test-table");

    HighScoreTable loaded;
    ASSERT_TRUE(loaded.load(path));
    ASSERT_EQ(loaded.entries().size(), 4u);
    EXPECT_EQ(loaded.entries()[0].score, 300u);
    EXPECT_EQ(loaded.entries()[1].score, 200u);
    EXPECT_EQ(loaded.entries()[1].initials, (std::array<char, 3>{'M', 'M', 'M'}));
    EXPECT_EQ(loaded.entries()[2].initials, (std::array<char, 3>{'N', 'N', 'N'}));
    EXPECT_EQ(loaded.entries()[3].score, 100u);
}

// ---- HighScores.SeedsDeclaredDefaultsElseStartsEmpty ----
TEST(HighScores, SeedsDeclaredDefaultsElseStartsEmpty) {
    TempDir dir("tb_seed_test");
    const auto path = dir.path / "seeded.json";

    // A pack declaring defaults seeds exactly those 10 on a fresh file.
    std::vector<table::DefaultScore> defaults;
    for (int i = 0; i < 10; ++i) {
        table::DefaultScore d;
        d.initials[0] = char('A' + i);
        d.initials[1] = 'A';
        d.initials[2] = 'A';
        d.score = uint64_t(10 - i) * 1'000'000;
        defaults.push_back(d);
    }
    HighScoreTable t;
    ASSERT_FALSE(t.load(path)); // missing → seed
    t.seed_defaults(defaults, "2026-08-27");
    t.save(path, "seeded");
    HighScoreTable reloaded;
    ASSERT_TRUE(reloaded.load(path));
    ASSERT_EQ(reloaded.entries().size(), 10u);
    EXPECT_EQ(reloaded.entries()[0].initials[0], 'A');
    EXPECT_EQ(reloaded.entries()[0].score, 10'000'000u);
    // The 11th score only qualifies above the 10th (§7).
    EXPECT_FALSE(reloaded.qualifies(1'000'000)); // ties do NOT beat
    EXPECT_TRUE(reloaded.qualifies(1'000'001));

    // A pack that declares none starts EMPTY: first posted score lands
    // at rank 1 — no built-in ladder (§7, binding).
    HighScoreTable empty;
    const auto empty_path = dir.path / "empty.json";
    ASSERT_FALSE(empty.load(empty_path));
    empty.seed_defaults({}, "2026-08-27");
    EXPECT_TRUE(empty.entries().empty());
    game::HighScoreEntry first;
    first.score = 500;
    first.initials = {'T', 'B', 'T'};
    first.date = "2026-08-27";
    EXPECT_EQ(empty.insert(first), 1);
    EXPECT_EQ(empty.entries().size(), 1u);
}

// ---- ExtraBall.SamePlayerShootsAgain ----
TEST(ExtraBall, SamePlayerShootsAgain) {
    Rig rig;
    ASSERT_NO_THROW(rig.load(kRules));
    HighScoreTable scores;
    GameMachine m(rig.host, *rig.state, scores, base_cfg());
    auto attach = [&m, &rig] {
        rig.state->fsm_ctx = &m;
        rig.state->fsm_step = [](void* ctx, sim::SimState& s, const sim::TickInput& in) {
            static_cast<GameMachine*>(ctx)->step(in);
        };
    };

    // Start, add a SECOND player (the single-player form of this test
    // masked the cycle-16 blocker: (0+1) % 1 == 0 keeps the player
    // current either way), reach BallInPlay for P1 ball 1.
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8);
    rig.step(attach, 0);
    rig.step(attach, 1u << 8); // P2 joins during P1 ball 1 (§3.1)
    ASSERT_EQ(m.player_count(), 2);
    for (int t = 0; t < 2'100 && m.state() != GameState::BallReady; ++t) {
        rig.step(attach, 0);
    }
    ASSERT_EQ(m.state(), GameState::BallReady);
    launch_ball(rig, m);
    rig.step(attach, 0);
    ASSERT_EQ(m.state(), GameState::BallInPlay);

    // Award an extra ball (script path), drain, and let the ball end.
    wait_out_ball_save(rig, m);
    rig.host.player_scores(1).extra_balls = 1; // tb.award_extra_ball path
    for (auto& b : rig.state->balls) {
        if (b.live) {
            b.pos = {0.2f, 0.012f};
            b.vel = {0.0f, 0.0f};
        }
    }
    for (int t = 0; t < 400'000 && m.state() == GameState::BallInPlay; ++t) {
        rig.step(attach, 0);
    }
    for (int t = 0; t < 400'000 && m.state() == GameState::BonusCount; ++t) {
        rig.step(attach, 0); // count-up / no-bonus display elapses
    }
    // BonusCount -> PlayerChange: same player, same ball number, no
    // player_up (11 §3.3).
    ASSERT_EQ(m.state(), GameState::PlayerChange);
    EXPECT_EQ(m.current_player(), 1);
    EXPECT_EQ(m.player(1).ball_number, 1); // replays ball 1
    EXPECT_EQ(rig.host.player_scores(1).extra_balls, 0);
    int64_t pu = 0;
    EXPECT_FALSE(rig.host.state_read_int(1, "pu", pu)); // never fired

    // And the next ball_start still says ball 1 for player 1.
    for (int t = 0; t < 400'000 && m.state() != GameState::BallInPlay; ++t) {
        if (m.state() == GameState::BallReady) {
            launch_ball(rig, m);
        }
        rig.step(attach, 0);
    }
    ASSERT_EQ(m.state(), GameState::BallInPlay);
    int64_t ball_no = 0;
    ASSERT_TRUE(rig.host.state_read_int(1, "last_ball", ball_no));
    EXPECT_EQ(ball_no, 1);
    ASSERT_TRUE(rig.host.state_read_int(1, "last_player", ball_no));
    EXPECT_EQ(ball_no, 1);
}
