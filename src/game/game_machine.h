#pragma once

#include "game/high_scores.h"
#include "game/initials_entry.h"
#include "sim/solver.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tb::game {

// 11-game-framework.md §2.1 — the state enum is owned by the spec.
enum class GameState : uint8_t {
    Boot,
    Attract,
    TableSelect,
    Settings,
    GameStarting,
    BallReady,
    BallInPlay,
    BonusCount,
    PlayerChange,
    HighScoreEntry,
    GameOver,
    Paused,
};

const char* state_name(GameState s);

// Framework configuration (settings + table meta; 11 §3.1/§4.3/§5).
struct FrameworkConfig {
    int balls_per_game = 3;            // settings game.balls_per_game
    int tilt_warnings = 2;             // settings game.tilt_warnings
    uint32_t ball_save_ticks = 8'000;  // settings game.ball_save_seconds
    uint64_t replay_score = 5'000'000; // meta.replay_score (11 §3.3)
    std::string date_stamp;            // YYYY-MM-DD, set once by the app
                                       // (wall clock never enters the sim, §1)
};

// One player's session state (11 §3.1). tb.state lives in the host.
struct PlayerState {
    uint64_t score = 0;
    int ball_number = 1;
    bool replay_awarded = false; // one replay per player per game (§3.3)
    // extra_balls lives in ONE place: the host's PlayerScoreState
    // (tb.award_extra_ball awards there; the framework's rotation and
    // the replay path read it).
};

// The game state machine (11-game-framework.md §2). Runs on the sim
// thread inside phase 3 of the 4-phase tick (§1): the solver invokes
// step() between script dispatch (phase 2) and timers (phase 4), so
// framework events dispatch to Lua synchronously in emission order.
//
// Tilt is not a state (§2.2): it is a per-ball flag inside BallInPlay.
// Menu states (TableSelect/Settings/Paused) are minimal placeholders
// until M18 (04-milestones.md M10 scope).
class GameMachine {
public:
    // Both references must outlive the machine; `host` must be loaded.
    GameMachine(sim::ScriptHost& host,
                sim::SimState& state,
                HighScoreTable& scores,
                const FrameworkConfig& cfg);

    // Phase 3 step. `input` is this tick's latched input (the same
    // TickInput the solver integrated; command edges are derived from
    // it — 05 §9.1 action bits, part of the replay record).
    void step(const sim::TickInput& input);

    GameState state() const { return state_; }

    int current_player() const { return current_player_ + 1; } // 1-based

    int player_count() const { return int(players_.size()); }

    const std::vector<PlayerState>& players() const { return players_; }

    const PlayerState& player(int i) const; // 1-based; asserts in range

    bool tilted() const { return tilted_; }

    int tilt_warning_count() const { return danger_count_; }

    const InitialsEntry& initials() const { return initials_; }

    uint64_t bonus_display() const { return bonus_display_; }

    bool ball_save_active() const;

    // True when the last game produced freshly-qualified entries (for
    // the backglass / tests).
    const std::vector<int>& qualifying_players() const { return qualifying_; }

    // Persistence seam: set when an initials commit inserted a row;
    // the app saves the file and clears the flag (§7: immediately
    // after each commit).
    bool scores_dirty() const { return scores_dirty_; }

    void clear_scores_dirty() { scores_dirty_ = false; }

    // Test seam: direct event injection (same path as phase-3 sim-event
    // consumption).
    void inject_sim_event(const sim::SimEvent& ev);

private:
    // ---- state entry helpers (§2.3) ----
    void enter(GameState next);
    void enter_attract();
    void enter_table_select();
    void enter_game_starting();
    void enter_ball_ready(bool new_ball);
    void enter_ball_in_play();
    void enter_bonus_count();
    void enter_player_change();
    void enter_high_score_entry();
    void enter_game_over();

    // ---- per-tick helpers ----
    void consume_sim_events();
    void on_drain(const sim::SimEvent& ev);
    void on_danger(const sim::SimEvent& ev);
    void on_ball_launched();
    void evaluate_t10();
    void command_serve(bool autolaunch);
    bool try_add_player(bool start_edge);          // §3.1 join window
    void play_named(const char* patch_name) const; // 12 §7.2 framework sounds
    void command_flipper_coil_restore();
    void do_tilt();
    void finish_bonus();
    bool session_over() const;
    void count_finished_ball(); // §3.1 accounting: number / extra ball
    void advance_rotation();    // §3.1 pointer: next player (game continues)
    int free_balls() const;
    int lane_balls() const;
    int held_balls() const;
    int locked_balls() const;
    bool lock_release_owed() const;

    bool serve_pending() const { return serve_pending_; }

    void check_multiball_edges();
    void check_replay(int player);
    void step_high_score_entry(bool start_edge, bool plunger_edge, bool left_edge, bool right_edge);
    void latch_action(sim::ScriptAction::Kind kind, bool flag = false);

    // Config + refs.
    sim::ScriptHost& host_;
    sim::SimState& s_;
    HighScoreTable& scores_;
    FrameworkConfig cfg_;

    // Machine state.
    GameState state_ = GameState::Boot;
    uint64_t state_ticks_ = 0; // ticks in the current state
    std::vector<PlayerState> players_;
    int current_player_ = 0;      // 0-based
    uint32_t prev_buttons_ = 0;   // command edge detection
    int prev_player_up_ = 0;      // previous_player for player_up
    bool fire_player_up_ = false; // deferred to PlayerChange's first tick

    // Ball lifecycle.
    bool serve_pending_ = false;
    bool serve_autolaunch_ = false;
    bool ball_launched_this_ball_ = false;
    bool ball_start_fired_ = false; // current ball's ball_start
    int save_uses_left_ = 0;
    uint32_t save_ticks_left_ = 0;
    int balls_in_play_at_mb_check_ = 0;
    uint32_t no_free_ball_ticks_ = 0; // §4.6 case B counter

    // Tilt (per ball).
    int danger_count_ = 0;
    bool tilted_ = false;

    // BonusCount staging.
    uint64_t bonus_display_ = 0;
    uint64_t bonus_target_ = 0;
    uint64_t bonus_step_ = 0;

    // HighScoreEntry.
    bool scores_dirty_ = false;
    InitialsEntry initials_;
    std::vector<int> qualifying_;
    size_t qualifying_idx_ = 0;

    // Paused placeholder.
    GameState paused_return_ = GameState::Attract;
    bool pause_latched_ = false; // pause key edge/hold bookkeeping

    // Add-player window (§3.1): P1 ball 1 before the first BonusCount.
    bool p1_bonus_seen_ = false;
    // §2.5 serve window: ticks since the last serve command.
    uint32_t serve_window_ticks_ = 0;
};

} // namespace tb::game
