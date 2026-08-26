#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Sim-thread Lua script host (10-scripting.md; 04-milestones.md M9).
// sol2/lua types never leak past this header — the sim sees dispatch,
// timers, and latched physical actions only. The load phase may throw;
// the hot path never throws and never allocates beyond the capped Lua
// heap.
namespace tb::sim {

struct SimState;
struct SimEvent;

// A physical action latched by a script call, applied at the start of the
// next tick's phase 1 (10-scripting.md §2.2).
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
    };

    Kind kind = Kind::Kick;
    uint16_t element = 0xFFFF; // TableDef element index
    float speed = 0.0f;        // kick speed / pulse ms (see kind)
    float angle_deg = 0.0f;    // kick angle
    int count = 0;             // release_lock count / add_ball n
    bool flag = false;         // set_flipper_enabled state
};

// Player-facing script state the framework/render read (scores, messages,
// backglass model). Written by the host on the sim thread.
struct PlayerScoreState {
    uint64_t score = 0;
    uint64_t bonus = 0;
    int bonus_multiplier = 1;
    int extra_balls = 0;
};

struct BackglassModel {
    // 10-scripting.md §3.7: declarative state the backglass reads
    // (rendered on the playfield debug overlay until M12).
    int layout = 0; // 0 scores, 1 mode, 2 celebration
    int focus_player = 1;
    std::string message;
    int message_style = 0; // 0 info, 1 mode, 2 jackpot, 3 warning
    uint32_t message_ticks_left = 0;
};

class ScriptHostImpl; // sol2-backed implementation (script_host.cpp)

class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();

    // Load phase: creates the sandboxed VM, binds tb.*, runs rules.lua's
    // top level + on_init. Throws std::runtime_error on compile/top-level
    // error (caught by the caller; table fails to load).
    void load(const std::string& rules_source, SimState& state);

    // Phase 2: dispatch one sim event (in emission order; handlers in
    // registration order). Never throws; §2.5 error containment.
    void dispatch(const SimEvent& event);

    // Phase 4: fire timers whose deadline == tick, ascending timer_id;
    // one incremental GC step.
    void on_tick(uint64_t tick);

    // Latched physical actions from any handler this tick (drained by the
    // solver at the start of the next tick).
    std::vector<ScriptAction>& pending_actions();

    // Framework hooks (11-game-framework.md arrives at M10; the host
    // exposes the swap now so M9 tests can drive players).
    void set_current_player(int index);
    void begin_game(int player_count);
    void end_game();

    // Sandbox introspection for tests.
    bool has_handler(const std::string& event_name) const;
    int watchdog_budget_remaining() const;

    // Bookkeeping the framework reads (per current player).
    PlayerScoreState& player_scores(int index);
    BackglassModel& backglass();

    // Deterministic script RNG access (rng_script stream; also used by
    // tb.rng/tb.rng_range).
    double rng_next();
    int rng_range(int lo, int hi);

private:
    void run_framework_handlers(const std::string& name, int payload);

    ScriptHostImpl* impl_ = nullptr;
};

} // namespace tb::sim
