#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Sim-thread Lua script host (10-scripting.md; 04-milestones.md M9).
// sol2/lua types never leak past this header — the sim sees the tick
// pipeline (begin_tick / dispatch / end_tick), latched physical actions,
// and bookkeeping state only. The load phase may throw; the hot path
// never throws and never allocates through operator new (Lua's own
// allocations go through the capped lua_Alloc, §1.1).
namespace tb::sim {

struct SimState;
struct SimEvent;
struct ScriptHostImpl; // sol-backed; defined in script_host.cpp
class MusicSink;       // music_sink.h — kept opaque here

// A physical action latched by a script call, applied by the solver at
// the start of the next tick's phase 1 (10-scripting.md §2.2).
struct ScriptAction {
    enum class Kind : uint8_t {
        Kick = 0,
        KickHold,
        ReleaseLock,
        MagnetOn,
        MagnetOff,
        MagnetPulse,
        SetFlipperEnabled,
        AddBall,
        DropBankReset,
        GateOpen,
        GateClose,
        // Framework commands (11-game-framework.md §5, M10): latched by
        // the GameMachine through the same drain as script actions.
        FlippersEnabled, // flag = gate state
        CoilsEnabled,    // flag = gate state
        ForceEjectAll,   // tilt/timeout: kick every hold, empty every
                         // lock (staggered), release magnets
        ResetDanger,     // 08 §7.3 end-of-ball danger reset
        LocksToTrough,   // §4.5 step 5: bookkeeping drain, no eject
    };

    Kind kind = Kind::Kick;
    uint16_t element = 0xFFFF; // TableDef element index
    float speed = 0.0f;        // kick speed / pulse ms (see kind)
    float angle_deg = 0.0f;    // kick angle
    int count = 0;             // release_lock count / add_ball n
    bool flag = false;         // set_flipper_enabled state
    bool use_speed = false;    // tb.kick override present
    bool use_angle = false;
};

// Player-facing score state the framework/render read. Written by the
// host on the sim thread.
struct PlayerScoreState {
    uint64_t score = 0;
    uint64_t bonus = 0;
    int bonus_multiplier = 1;
    int extra_balls = 0;
};

// tb.backglass / tb.show_message target (10-scripting.md §3.7). Rendered
// on the playfield debug overlay until M12. Fixed buffers: the message
// path stays allocation-free.
struct BackglassModel {
    static constexpr size_t kMessageCap = 64;

    int layout = 0; // 0 scores, 1 mode, 2 celebration
    int focus_player = 1;
    char message[kMessageCap + 1]{};
    uint32_t message_len = 0;
    int message_style = 0; // 0 info, 1 mode, 2 jackpot, 3 warning
    uint32_t message_ticks_left = 0;
};

// Framework-event payloads for fire_event (11-game-framework.md's FSM is
// the M10 caller; tests drive it until then).
using EventInts = std::vector<std::pair<std::string, int64_t>>;
using EventStrings = std::vector<std::pair<std::string, std::string>>;
using EventIntArrays = std::vector<std::pair<std::string, std::vector<int64_t>>>;

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Load phase: creates the sandboxed VM, binds tb.*, runs rules.lua's
    // top level + on_init. Throws std::runtime_error on compile/top-level
    // error (caller fails the table clean; 03-process.md §1.6). `state`
    // must outlive the host.
    void load(const std::string& rules_source, SimState& state);

    // Tick pipeline (10-scripting.md §2.2), called by the solver in order.
    void begin_tick(uint64_t tick);       // phase 2 start: budget reset
    void dispatch(const SimEvent& event); // phase 2: one sim event
    void end_tick(uint64_t tick);         // phase 4: timers + one GC step

    // Framework seam: dispatch a framework-originated event (phase 3
    // semantics — synchronous). Unknown names warn and no-op.
    void fire_event(const char* name,
                    const EventInts& ints = {},
                    const EventStrings& strings = {},
                    const EventIntArrays& arrays = {});

    void begin_game(int player_count);
    void end_game();
    void set_current_player(int index);
    void set_timers_frozen(bool frozen); // §3.6: freeze outside BALL_IN_PLAY

    // Music seam (12-audio.md §9, M14): tb.play_music/stop_music call
    // straight through on the sim thread. Null = the API warns and
    // no-ops (tests / attract, where no script runs at all).
    void set_music_sink(MusicSink* sink);

    // Framework seams (M10, 11-game-framework.md).
    void add_player();              // mid-game join during P1 ball 1
    void set_ball_number(int ball); // tb.game.ball_number (extra ball
                                    // replays the same number)
    void cancel_all_timers();       // §4.5 step 4: end-of-ball cancel
    void set_ledger_frozen(bool);   // §5 tilt / §4.5 step 1: discard
                                    // tb.score/add_bonus posts

    // Latched physical actions (drained by the solver at next phase 1).
    std::vector<ScriptAction>& pending_actions();

    PlayerScoreState& player_scores(int index);
    BackglassModel& backglass();

    bool has_handler(const std::string& event_name) const;
    int watchdog_budget_remaining() const;
    bool scripting_active() const;

    // Test seam: read a player's tb.state backing table (int/number
    // values only). Returns false when the key is absent.
    bool state_read_int(int player, const char* key, int64_t& out) const;

    // Test seam: string values (element ids, tags).
    bool state_read_string(int player, const char* key, std::string& out) const;

private:
    ScriptHostImpl* impl_ = nullptr;
};

} // namespace tb::sim
