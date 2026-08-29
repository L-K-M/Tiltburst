#include "game/game_machine.h"

#include "core/log.h"
#include "game/score_format.h"
#include "sim/script_host.h"

#include <algorithm>
#include <cassert>

namespace tb::game {

using sim::BallMode;
using sim::ScriptAction;
using sim::SimEventType;

// 05 §9.1 action indices (command bits the FSM consumes).
static constexpr uint32_t kBitLeftFlipper = 0;
static constexpr uint32_t kBitRightFlipper = 1;
static constexpr uint32_t kBitPlunger = 4;
static constexpr uint32_t kBitStart = 8;
static constexpr uint32_t kBitPause = 9;

// 11-game-framework.md tick constants (1 s = 1000 ticks).
static constexpr uint32_t kIntroTicks = 2'000;       // T8 / T16
static constexpr uint32_t kBonusCountTicks = 2'000;  // §4.5 step 3
static constexpr uint32_t kTiltDisplayTicks = 1'500; // §4.5 step 3
static constexpr uint32_t kNoBonusDisplayTicks = 800;
static constexpr uint32_t kGameOverTimeoutTicks = 10'000; // T19
static constexpr uint32_t kServeWindowTicks = 9'000;      // §2.5 bound
static constexpr uint32_t kLaneStallTicks = 30'000;       // §4.2/§4.6 case B
static constexpr int kBonusSteps = 25;                    // §4.5 step 3

const char* state_name(GameState s) {
    switch (s) {
    case GameState::Boot:
        return "Boot";
    case GameState::Attract:
        return "Attract";
    case GameState::TableSelect:
        return "TableSelect";
    case GameState::Settings:
        return "Settings";
    case GameState::GameStarting:
        return "GameStarting";
    case GameState::BallReady:
        return "BallReady";
    case GameState::BallInPlay:
        return "BallInPlay";
    case GameState::BonusCount:
        return "BonusCount";
    case GameState::PlayerChange:
        return "PlayerChange";
    case GameState::HighScoreEntry:
        return "HighScoreEntry";
    case GameState::GameOver:
        return "GameOver";
    case GameState::Paused:
        return "Paused";
    }
    return "?";
}

GameMachine::GameMachine(sim::ScriptHost& host,
                         sim::SimState& state,
                         HighScoreTable& scores,
                         const FrameworkConfig& cfg)
    : host_(host), s_(state), scores_(scores), cfg_(cfg) {
    // T1: one-shot init happened (constructor = Boot); the table scan
    // is the app's job. Enter Attract immediately — the framework never
    // runs a script here (§8.2: no Lua attract hook; the sim keeps
    // stepping but nothing listens until a game starts).
    enter(GameState::Attract);
}

void GameMachine::latch_action(ScriptAction::Kind kind, bool flag) {
    ScriptAction a;
    a.kind = kind;
    a.flag = flag;
    host_.pending_actions().push_back(a);
}

void GameMachine::enter(GameState next) {
    // §2.3 exit column: leaving Attract stops the attract music (the
    // script's ball_start -> main crossfade owns the game-side handoff)
    // and drops the framework light show off the table's lights.
    if (state_ == GameState::Attract && next != GameState::Attract) {
        if (music_sink_ != nullptr) {
            music_sink_->stop_music();
        }
        for (sim::LightState& light : s_.lights) {
            light.on = false;
        }
    }
    state_ = next;
    state_ticks_ = 0;
    switch (next) {
    case GameState::Attract:
        enter_attract();
        break;
    case GameState::TableSelect:
        enter_table_select();
        break;
    case GameState::GameStarting:
        enter_game_starting();
        break;
    case GameState::BallReady:
        enter_ball_ready(true);
        break;
    case GameState::BallInPlay:
        enter_ball_in_play();
        break;
    case GameState::BonusCount:
        enter_bonus_count();
        break;
    case GameState::PlayerChange:
        enter_player_change();
        break;
    case GameState::HighScoreEntry:
        enter_high_score_entry();
        break;
    case GameState::GameOver:
        enter_game_over();
        break;
    case GameState::Paused:
        host_.set_timers_frozen(true); // §8.5: everything tick-based freezes
        break;
    default:
        break; // Boot/Settings have no M10 entry work
    }
}

// ---- state entries (§2.3) ----

void GameMachine::enter_attract() {
    // §8.2: no script runs in Attract. M10 keeps the loaded host (the
    // full table release/reload cycle is the app's M14 job); freezing
    // timers + ledger pins everything that could mutate game state.
    host_.set_timers_frozen(true);
    host_.set_ledger_frozen(true);
    command_flipper_coil_restore();
    // Page machine restarts (§8.2) and the framework auto-plays the
    // attract song of the loaded table (12 §9) — the sink resolves
    // nothing for a song-less table, which is legal silence.
    attract_page_ = 0;
    attract_page_ticks_ = 0;
    if (music_sink_ != nullptr) {
        music_sink_->play_music("attract");
    }
}

void GameMachine::step_attract(bool left_edge, bool right_edge) {
    // §8.2 page rotation: Logo 8 s / high scores 2x4 s / rules card
    // 10 s / press start 5 s; manual paging via flippers resets the
    // page timer.
    static constexpr uint32_t kPageTicks[kAttractPages] = {8000, 4000, 4000, 10000, 5000};
    if (left_edge) {
        attract_page_ = (attract_page_ + int(kAttractPages) - 1) % int(kAttractPages);
        attract_page_ticks_ = 0;
    }
    if (right_edge) {
        attract_page_ = (attract_page_ + 1) % int(kAttractPages);
        attract_page_ticks_ = 0;
    }
    if (++attract_page_ticks_ >= kPageTicks[size_t(attract_page_)]) {
        attract_page_ = (attract_page_ + 1) % int(kAttractPages);
        attract_page_ticks_ = 0;
    }
    // Framework light show (13 §7.2 v1): a fixed routine over the
    // loaded table's lights — no Lua runs in Attract (§8.2). Sim ticks
    // keep wall-clock pace here (nothing freezes the integrator in
    // Attract), so tick ms == wall ms. Function-tag / light-group
    // personalization lands with the tables that declare them
    // (M16/M17 authoring); v1 runs the loop over every light.
    const uint64_t ms = state_ticks_; // in-state clock, 1 tick == 1 ms
    const uint32_t phase = uint32_t(ms % 15000);
    const size_t n = s_.lights.size();
    for (size_t li = 0; li < n; ++li) {
        sim::LightState& light = s_.lights[li];
        bool on = false;
        if (phase < 8000) {
            // Step 1: breathe, phase-offset bottom-to-top across five
            // bands of table_h/5 (0.208 m on the default field), so
            // the wave climbs the table (§7.2 step 1). The continuous
            // clock (no % 15000 wrap) keeps the wave phase-smooth.
            const int band = std::clamp(int(light.pos.y / 0.208f), 0, 4);
            const double local = double(ms) * 0.001 - 0.4 * double(band) + 8.0;
            on = std::fmod(local, 1.0) < 0.5; // 1 Hz breath
        } else if (phase < 12000) {
            // Step 2: chase along declaration order at the §7.1
            // 80 ms/lamp rate.
            on = n > 0 && li == size_t(((ms - 8000) / 80) % n);
        } else if (phase < 13000) {
            // Step 3: strobe (fast regular blink).
            on = ((ms / 62u) & 1u) != 0u;
        }
        // Step 4 (13000-15000): dark beat — everything off.
        light.on = on;
    }
}

void GameMachine::enter_table_select() {
    // Placeholder until M18 (04-milestones.md): single table loaded, no
    // carousel yet.
}

void GameMachine::enter_game_starting() {
    // Fresh session (§3.1): one player; Start presses during P1 ball 1
    // add players (§3.1). ALL machine + host state is initialized
    // BEFORE begin_game — it fires game_start synchronously, and the
    // same ScriptHost persists across games (the handlers must not see
    // the previous session's player).
    players_.assign(1, PlayerState{});
    current_player_ = 0;
    prev_player_up_ = 0;
    tilted_ = false;
    danger_count_ = 0;
    serve_pending_ = false;
    p1_bonus_seen_ = false;
    fire_player_up_ = false;
    ball_start_fired_ = false;
    balls_in_play_at_mb_check_ = 0;
    host_.set_current_player(1);
    // Lift any residual freeze BEFORE begin_game: game_start fires
    // synchronously and its handlers may score (§6). On a back-to-back
    // game the previous session's ball_end/tilt freeze is still set.
    host_.set_ledger_frozen(false);
    host_.begin_game(1);
    host_.set_timers_frozen(true);
    command_flipper_coil_restore();
}

void GameMachine::enter_ball_ready(bool new_ball) {
    // Lift any residual freeze (§6 symmetry: the ball_end freeze
    // bracketed the PREVIOUS ball; ball_start handlers may score).
    host_.set_ledger_frozen(false);
    // Fire ball_start for new balls only (not saves/adds — §2.3).
    if (new_ball && !ball_start_fired_) {
        PlayerState& p = players_[size_t(current_player_)];
        host_.set_ball_number(p.ball_number);
        host_.fire_event("ball_start",
                         {{"player", current_player() + 0}, {"ball_number", p.ball_number}});
        ball_start_fired_ = true;
    }
    ball_launched_this_ball_ = false;
    // Default save arms at launch (§4.3), not here.
}

void GameMachine::enter_ball_in_play() {
    if (!ball_launched_this_ball_) {
        ball_launched_this_ball_ = true;
        // ball_launched itself is a sim event (08 §6.16) the scripts
        // already saw in phase 2 — the framework never re-fires it.
        // §4.3: single-use default save counts from ball_launched.
        if (cfg_.ball_save_ticks > 0) {
            save_uses_left_ = 1;
            save_ticks_left_ = cfg_.ball_save_ticks;
        }
    }
    host_.set_timers_frozen(false); // §3.6: live in play
}

void GameMachine::enter_bonus_count() {
    p1_bonus_seen_ = true;
    // §4.5 step 4: cancel all running script timers.
    host_.cancel_all_timers();
    host_.set_timers_frozen(true);
    // Step 2: ball_end carries the bonus as of emission; the handler
    // may still add_bonus (last chance). The sustained freeze goes on
    // after the handler returns (§6).
    PlayerState& p = players_[size_t(current_player_)];
    host_.set_ball_number(p.ball_number);
    host_.fire_event(
        "ball_end",
        {{"player", current_player() + 0},
         {"ball_number", p.ball_number},
         {"bonus", int64_t(host_.player_scores(current_player()).bonus)},
         {"bonus_multiplier", host_.player_scores(current_player()).bonus_multiplier}});
    host_.set_ledger_frozen(true); // §6: after ball_end returns
    // Step 3 staging: tilt forfeits the bonus (§5).
    const uint64_t bonus = host_.player_scores(current_player()).bonus;
    const int mult = host_.player_scores(current_player()).bonus_multiplier;
    bonus_target_ = tilted_ ? 0 : bonus * uint64_t(mult);
    bonus_display_ = 0;
    bonus_step_ = 0;
    // Step 5 (lock drain part): bookkeeping transfer to the trough, no
    // eject and no drain event.
    latch_action(ScriptAction::Kind::LocksToTrough);
}

void GameMachine::enter_player_change() {
    // §2.3: the rotation already advanced in finish_bonus (the
    // finishing player's number had to count before the session
    // check); this entry does the tb.state swap, and player_up fires
    // on the state's first tick (below). Extra ball: same player
    // replays the same number and no player_up fires.
    host_.set_current_player(current_player());
    fire_player_up_ = true;
}

void GameMachine::enter_high_score_entry() {
    qualifying_.clear();
    for (int p = 1; p <= player_count(); ++p) {
        if (scores_.qualifies(host_.player_scores(p).score)) {
            qualifying_.push_back(p);
        }
    }
    qualifying_idx_ = 0;
    if (qualifying_.empty()) {
        enter(GameState::GameOver); // defensive; T14 guarantees non-empty
        return;
    }
    initials_.begin();
}

void GameMachine::enter_game_over() {
    host_.end_game(); // fires game_end {winner, scores} synchronously
    // §9: the framework plays the game_over song on game end (a table
    // without one stays silent — legal subset).
    if (music_sink_ != nullptr) {
        music_sink_->play_music("game_over");
    }
}

// ---- per-tick machinery ----

int GameMachine::free_balls() const {
    int n = 0;
    for (const sim::Ball& b : s_.balls) {
        if (b.live && b.mode == BallMode::Free) {
            ++n;
        }
    }
    return n;
}

int GameMachine::lane_balls() const {
    // Balls resting in the plunger lane block T10 (§2.5).
    if (!s_.has_plunger) {
        return 0;
    }
    int n = 0;
    for (const sim::Ball& b : s_.balls) {
        if (b.live && b.mode == BallMode::Free && length(b.pos - s_.plunger.pos) < 0.06f) {
            ++n;
        }
    }
    return n;
}

int GameMachine::held_balls() const {
    int n = 0;
    for (const sim::Ball& b : s_.balls) {
        if (b.live && b.mode == BallMode::Captured) {
            ++n;
        }
    }
    return n;
}

int GameMachine::locked_balls() const {
    return s_.locked_balls;
}

bool GameMachine::lock_release_owed() const {
    // §2.5 fifth condition: a release sequence still owing an eject.
    for (const sim::BallLockElem& lock : s_.ball_locks) {
        if (lock.release_pending > 0 || lock.release_timer > 0) {
            return true;
        }
    }
    return false;
}

const PlayerState& GameMachine::player(int i) const {
    assert(i >= 1 && i <= player_count());
    return players_[size_t(i - 1)];
}

bool GameMachine::try_add_player(bool start_edge) {
    // §3.1: joins are legal only while player 1 is on ball 1, before
    // the first BonusCount (states GameStarting/BallReady/BallInPlay).
    // players_.empty(): the machine exists before any session (Attract
    // never calls this today, but the guard keeps the index safe
    // against future call sites).
    if (!start_edge || players_.empty() || player_count() >= 4 || current_player_ != 0 ||
        players_[0].ball_number != 1 || p1_bonus_seen_) {
        return false;
    }
    host_.add_player();
    players_.push_back(PlayerState{});
    return true;
}

void GameMachine::command_serve(bool autolaunch) {
    ScriptAction a;
    a.kind = ScriptAction::Kind::AddBall;
    a.count = 1;
    host_.pending_actions().push_back(a);
    serve_pending_ = true;
    serve_autolaunch_ = autolaunch;
    if (s_.has_plunger) {
        s_.plunger.auto_launch = autolaunch; // fires 500 ticks after settle
    }
    serve_window_ticks_ = 0;
}

void GameMachine::command_flipper_coil_restore() {
    latch_action(ScriptAction::Kind::FlippersEnabled, true);
    latch_action(ScriptAction::Kind::CoilsEnabled, true);
}

void GameMachine::do_tilt() {
    tilted_ = true;
    host_.fire_event("tilt");
    // §5: multiball edges are suspended for the tilted ball — freeze
    // the tracker so the suspension cannot fire a phantom end edge
    // next ball.
    balls_in_play_at_mb_check_ = free_balls() + held_balls();
    // §5 consequences: flippers + coils dead, ledger frozen, save
    // cancelled, every captured ball force-ejected (the sim staggers
    // lock emptying; T10's fifth condition holds the ball open until
    // every owed eject lands).
    latch_action(ScriptAction::Kind::FlippersEnabled, false);
    latch_action(ScriptAction::Kind::CoilsEnabled, false);
    host_.set_ledger_frozen(true);
    save_uses_left_ = 0;
    save_ticks_left_ = 0;
    latch_action(ScriptAction::Kind::ForceEjectAll);
}

void GameMachine::on_danger(const sim::SimEvent& ev) {
    if (tilted_) {
        return; // per ball; nothing more accrues once tilted
    }
    // §5: the first cfg.tilt_warnings events are warnings; the next is
    // TILT. crossing_index lives in the high half of ev.data (diagnostic
    // drop/dup detection is the test suite's job).
    ++danger_count_;
    if (danger_count_ <= cfg_.tilt_warnings) {
        host_.fire_event("tilt_warning", {{"count", danger_count_}});
    } else {
        do_tilt();
    }
}

void GameMachine::on_ball_launched() {
    // T9: one-way into BallInPlay.
    if (state_ == GameState::BallReady) {
        enter(GameState::BallInPlay);
    }
}

void GameMachine::on_drain(const sim::SimEvent&) {
    // Accounting only here (T11); the T10/T12 decisions are
    // re-evaluated every drain in evaluate_t10().
    // T12 applies to a ball-ENDING drain: a multiball drain that
    // leaves a ball up is T11 — drain event only, no serve (the
    // 3-ball multiball done-when, 16 §2).
    // Mirrors evaluate_t10's keep-open set exactly: a held (kicker/
    // magnet) or locked ball keeps the ball open, so that drain is T11
    // — no save, no serve (an extra free ball would appear when the
    // hold releases).
    const bool ball_ending = free_balls() == 0 && lane_balls() == 0 && held_balls() == 0 &&
                             locked_balls() == 0 && !serve_pending() && !lock_release_owed();
    if (save_uses_left_ > 0 && save_ticks_left_ > 0 && !tilted_ && ball_ending) {
        --save_uses_left_;
        save_ticks_left_ = 0;
        command_serve(true);
        return;
    }
}

void GameMachine::evaluate_t10() {
    if (state_ != GameState::BallInPlay) {
        return;
    }
    // T10: the ONLY route out of play. Five spec conditions (§2.5) plus
    // the §4.6 reading: held (kicker/magnet) and locked balls keep the
    // ball open — case B's 30 s watchdog recovers them, so the machine
    // must not end the ball out from under a hold.
    if (free_balls() > 0 || lane_balls() > 0 || serve_pending() ||
        (save_uses_left_ > 0 && save_ticks_left_ > 0) || lock_release_owed() || held_balls() > 0 ||
        locked_balls() > 0) {
        return;
    }
    // No free ball and none owed anywhere: this drain (or a force-eject
    // chain) has ended the ball.
    enter(GameState::BonusCount);
}

void GameMachine::check_multiball_edges() {
    // §4.4: edge-triggered on free + held rising above 1; back to 1
    // ends. Suspended for the remainder of a tilted ball (§5).
    if (tilted_) {
        return;
    }
    const int active = free_balls() + held_balls();
    if (active > 1 && balls_in_play_at_mb_check_ <= 1) {
        host_.fire_event("multiball_start", {{"ball_count", active}});
    } else if (active <= 1 && balls_in_play_at_mb_check_ > 1) {
        // <= 1, not == 1: a same-tick 2→0 double drain still fires the
        // end edge before T10 (§2.5).
        host_.fire_event("multiball_end");
    }
    balls_in_play_at_mb_check_ = active;
}

void GameMachine::check_replay(int player) {
    // §3.3: one replay per player per game at the threshold; award is
    // an extra ball (default) — "off" strips the config to a zero
    // threshold upstream.
    PlayerState& p = players_[size_t(player - 1)];
    if (p.replay_awarded || cfg_.replay_score == 0) {
        return;
    }
    if (host_.player_scores(player).score >= cfg_.replay_score) {
        auto& ps = host_.player_scores(player);
        if (ps.extra_balls < 3) {
            p.replay_awarded = true; // one replay per player per game
            ++ps.extra_balls;
        } else if (!tilted_) {
            p.replay_awarded = true;
            ps.score = std::min<uint64_t>(ps.score + 100'000ull, kScoreCap); // §3.3
        }
        // Tilted AND at the cap: nothing granted, the threshold
        // crossing is NOT consumed (checked again next tick).
    }
}

void GameMachine::consume_sim_events() {
    // Phase 3 reads the same per-tick log the scripts saw in phase 2
    // (emission order preserved).
    for (size_t i = 0; i < s_.tick_event_n; ++i) {
        inject_sim_event(s_.tick_events[i]);
    }
}

void GameMachine::inject_sim_event(const sim::SimEvent& ev) {
    switch (SimEventType(ev.type)) {
    case SimEventType::DangerThreshold:
        on_danger(ev);
        break;
    case SimEventType::BallLaunched:
        on_ball_launched();
        break;
    case SimEventType::Drain:
        on_drain(ev);
        break;
    case SimEventType::BallServed:
        serve_pending_ = false; // serve window closed (§2.5)
        // The autolaunch arm served exactly this ball (§4.2); a normal
        // player plunge owns the lane from here on.
        if (s_.has_plunger) {
            s_.plunger.auto_launch = false;
        }
        break;
    default:
        break; // element events are script domain (phase 2)
    }
}

bool GameMachine::ball_save_active() const {
    return save_uses_left_ > 0 && save_ticks_left_ > 0 && !tilted_;
}

bool GameMachine::session_over() const {
    // §3.1: every player finished ball balls_per_game with no extra
    // balls pending (the extra-ball count is the host's).
    for (size_t i = 0; i < players_.size(); ++i) {
        if (players_[i].ball_number <= cfg_.balls_per_game ||
            host_.player_scores(int(i) + 1).extra_balls > 0) {
            return false;
        }
    }
    return true;
}

void GameMachine::count_finished_ball() {
    // §3.1's accounting half: a PENDING extra ball replays the same
    // player and number ("SHOOT AGAIN") — the extra ball itself is
    // consumed by advance_rotation, which keeps the player current;
    // otherwise the finishing player's own ball_number advances. The
    // extra-ball count is the host's (award path = tb.award_extra_ball).
    if (host_.player_scores(current_player()).extra_balls > 0) {
        return; // SHOOT AGAIN: same player, same number
    }
    players_[size_t(current_player_)].ball_number += 1;
}

void GameMachine::advance_rotation() {
    // §3.1's pointer half. A pending extra ball keeps the SAME player
    // (consumed here — the decrement must live with the pointer
    // decision or the SHOOT AGAIN signal is destroyed before it is
    // read, cycle-16 blocker). Otherwise next player is p % N + 1,
    // skipping players who finished all their balls with no extras
    // pending (their own ball_number is what counts — mid-game joins
    // included). Only called when the game continues; a finishing game
    // keeps the last player current for game_end.
    auto& ps = host_.player_scores(current_player());
    if (ps.extra_balls > 0) {
        --ps.extra_balls;
        return; // SHOOT AGAIN
    }
    const int n = int(players_.size());
    for (int i = 0; i < n; ++i) {
        current_player_ = (current_player_ + 1) % n;
        const PlayerState& cand = players_[size_t(current_player_)];
        if (cand.ball_number <= cfg_.balls_per_game ||
            host_.player_scores(current_player()).extra_balls > 0) {
            return; // this player still owes a ball
        }
    }
    // Unreachable when !session_over(): at least one candidate exists.
    TB_LOG_WARN("game", "rotation found no unfinished player");
}

void GameMachine::finish_bonus() {
    // §4.5 step 5: reset per-ball state. Danger, flags and the ledger
    // come back; whatever is still locked already drained to the trough
    // (enter_bonus_count latched LocksToTrough).
    latch_action(ScriptAction::Kind::ResetDanger);
    command_flipper_coil_restore();
    host_.set_ledger_frozen(false);
    auto& ps = host_.player_scores(current_player());
    ps.bonus = 0;
    ps.bonus_multiplier = 1;
    tilted_ = false;
    danger_count_ = 0;
    save_uses_left_ = 0;
    save_ticks_left_ = 0;
    ball_start_fired_ = false;
    balls_in_play_at_mb_check_ = 0;

    // §3.3: bonus collection can cross the replay threshold — check
    // before the session decision, so the last ball's extra ball still
    // replays (advance_rotation consumes it below).
    check_replay(current_player());
    // Count the finished ball FIRST: the finishing player's number
    // must include it before the session check; the player pointer
    // only advances when the game continues (GameOver keeps the last
    // player current for game_end).
    count_finished_ball();
    if (session_over()) {
        // T14/T15: qualification is against the live table.
        bool any = false;
        for (int p = 1; p <= player_count() && !any; ++p) {
            any = scores_.qualifies(host_.player_scores(p).score);
        }
        enter(any ? GameState::HighScoreEntry : GameState::GameOver);
    } else {
        advance_rotation();
        enter(GameState::PlayerChange);
    }
}

// ---- HighScoreEntry driving (§7) ----

void GameMachine::step_high_score_entry(bool start_edge,
                                        bool plunger_edge,
                                        bool left_edge,
                                        bool right_edge) {
    if (qualifying_idx_ >= qualifying_.size()) {
        enter(GameState::GameOver);
        return;
    }
    initials_.tick();
    if (right_edge) {
        initials_.next_glyph();
    }
    if (left_edge) {
        initials_.prev_glyph();
    }
    if (plunger_edge) {
        initials_.backspace();
    }
    if (start_edge) {
        initials_.confirm();
    }
    if (initials_.done()) {
        const int player = qualifying_[qualifying_idx_];
        HighScoreEntry entry;
        entry.initials = initials_.initials();
        entry.score = host_.player_scores(player).score;
        entry.date = cfg_.date_stamp; // stamped once by the app (§1)
        const int rank = scores_.insert(entry);
        scores_dirty_ = true;
        if (rank == 1) {
            TB_LOG_INFO("game", "grand champion: player {}", player);
        }
        ++qualifying_idx_;
        if (qualifying_idx_ < qualifying_.size()) {
            initials_.begin();
        } else {
            enter(GameState::GameOver); // T17
        }
    }
}

// ---- main step ----

void GameMachine::step(const sim::TickInput& input) {
    const uint32_t rising = input.buttons & ~prev_buttons_;
    prev_buttons_ = input.buttons;
    const bool start_edge = (rising >> kBitStart) & 1u;
    const bool plunger_edge = (rising >> kBitPlunger) & 1u;
    const bool left_edge = (rising >> kBitLeftFlipper) & 1u;
    const bool right_edge = (rising >> kBitRightFlipper) & 1u;
    const bool pause_held = (input.buttons >> kBitPause) & 1u;

    ++state_ticks_;

    // Sim events first: drains/launches/danger drive most transitions.
    consume_sim_events();

    switch (state_) {
    case GameState::Attract:
        step_attract(left_edge, right_edge);
        if (start_edge) {
            enter(GameState::TableSelect); // T3
        }
        break;

    case GameState::TableSelect:
        // Placeholder (M18 owns the carousel): Start starts a standard
        // 1-player game on the loaded table; plunger goes back (T6).
        if (start_edge) {
            enter(GameState::GameStarting); // T5
        } else if (plunger_edge) {
            enter(GameState::Attract); // T6
        }
        break;

    case GameState::GameStarting: {
        // Start during P1 ball 1 adds a player (§3.1).
        try_add_player(start_edge);
        if (state_ticks_ >= kIntroTicks) {
            command_serve(false); // T8: trough serve commanded
            enter(GameState::BallReady);
        }
        break;
    }

    case GameState::BallReady: {
        try_add_player(start_edge);
        if (pause_held && !pause_latched_) {
            pause_latched_ = true;
            paused_return_ = GameState::BallReady;
            enter(GameState::Paused); // T20
            break;
        }
        if (!pause_held) {
            pause_latched_ = false;
        }
        // T9 fires from the BallLaunched sim event (on_ball_launched).
        break;
    }

    case GameState::BallInPlay: {
        try_add_player(start_edge);
        if (pause_held && !pause_latched_) {
            pause_latched_ = true;
            paused_return_ = GameState::BallInPlay;
            enter(GameState::Paused); // T20
            break;
        }
        if (!pause_held) {
            pause_latched_ = false;
        }

        // Ball save countdown (§4.3).
        if (save_ticks_left_ > 0) {
            --save_ticks_left_;
            if (save_ticks_left_ == 0 && save_uses_left_ > 0) {
                save_uses_left_ = 0;
                host_.fire_event("ball_save_expired");
            }
        }
        // Serve window bookkeeping (§2.5): bounded at 9000 ticks.
        if (serve_pending_) {
            if (++serve_window_ticks_ >= kServeWindowTicks) {
                TB_LOG_WARN("game", "serve window exceeded; re-commanding");
                command_serve(serve_autolaunch_);
            }
        }
        // §4.2 lane-stall safety + §4.6 case B: zero free balls with
        // everything parked (lane hold or captured) for 30 s gets a
        // framework nudge.
        if (free_balls() == 0) {
            if (++no_free_ball_ticks_ == kLaneStallTicks) {
                TB_LOG_ERROR("game",
                             "stuck-ball case B: no free ball for 30 s; "
                             "force-ejecting every hold");
                latch_action(ScriptAction::Kind::ForceEjectAll);
                if (s_.has_plunger && lane_balls() > 0) {
                    s_.plunger.auto_launch = true; // lane stall autolaunch
                }
            } else if (no_free_ball_ticks_ > kLaneStallTicks) {
                no_free_ball_ticks_ = 0; // one shot per 30 s window
            }
        } else {
            no_free_ball_ticks_ = 0;
        }

        check_multiball_edges();
        check_replay(current_player());
        evaluate_t10();
        break;
    }

    case GameState::BonusCount: {
        // §4.5 step 3: count the product in at most 25 visible steps
        // over 2000 ticks; tilt shows TILT and collects 0; both
        // flippers held collects instantly.
        const uint32_t duration =
            tilted_ ? kTiltDisplayTicks
                    : (bonus_target_ == 0 ? kNoBonusDisplayTicks : kBonusCountTicks);
        const bool skip = (input.buttons & ((1u << kBitLeftFlipper) | (1u << kBitRightFlipper))) ==
                          ((1u << kBitLeftFlipper) | (1u << kBitRightFlipper));
        if (state_ticks_ >= duration || (skip && !tilted_)) {
            // Collect any remainder instantly.
            if (bonus_display_ < bonus_target_) {
                host_.player_scores(current_player()).score = std::min(
                    host_.player_scores(current_player()).score + (bonus_target_ - bonus_display_),
                    kScoreCap);
                bonus_display_ = bonus_target_;
            }
            finish_bonus(); // T13/T14/T15
            break;
        }
        const uint64_t due =
            std::min(bonus_target_, bonus_target_ * uint64_t(state_ticks_) / duration);
        if (due > bonus_display_) {
            host_.player_scores(current_player()).score = std::min(
                host_.player_scores(current_player()).score + (due - bonus_display_), kScoreCap);
            bonus_display_ = due;
        }
        break;
    }

    case GameState::PlayerChange: {
        // §2.3: player_up at entry when the player changed (an
        // extra-ball replay fires none), then the 2000-tick display
        // and the serve (T16).
        if (fire_player_up_) {
            fire_player_up_ = false;
            if (current_player() != prev_player_up_) {
                host_.fire_event(
                    "player_up",
                    {{"player", current_player()}, {"previous_player", prev_player_up_}});
            }
            prev_player_up_ = current_player();
        }
        if (state_ticks_ >= kIntroTicks) {
            command_serve(false);
            enter(GameState::BallReady);
        }
        break;
    }

    case GameState::HighScoreEntry:
        step_high_score_entry(start_edge, plunger_edge, left_edge, right_edge);
        break;

    case GameState::GameOver:
        if (start_edge) {
            enter(GameState::GameStarting); // T18: same table, 1 player
        } else if (state_ticks_ >= kGameOverTimeoutTicks || plunger_edge) {
            enter(GameState::Attract); // T19
        }
        break;

    case GameState::Paused:
        // Placeholder (M18 owns the menu): Start resumes into the
        // frozen state (T22) without re-running its entry actions —
        // the state was never exited, only frozen.
        if (start_edge) {
            state_ = paused_return_;
            state_ticks_ = 0;
            host_.set_timers_frozen(state_ != GameState::BallInPlay);
        }
        break;

    default:
        break;
    }
}

} // namespace tb::game
