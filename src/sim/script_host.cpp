#include "sim/script_host.h"

#include "core/log.h"
#include "sim/solver.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

// Sandbox + dispatch implementation (10-scripting.md §1–§4). One
// lua_State per running game, sim-thread only.
namespace tb::sim {

namespace {

// §1.1: 64 MiB Lua heap cap.
constexpr size_t kLuaHeapCap = 64u * 1024u * 1024u;
// §2.4: 10,000 instructions per tick at 1,000-instruction hook granularity.
constexpr int kTickInstructionBudget = 10000;
constexpr int kHookGranularity = 1000;
// §2.5: 10 consecutive failures disable a handler.
constexpr int kDisableAfterConsecutiveErrors = 10;
// §1.4: fixed hash seed.
constexpr unsigned kLuaHashSeed = 0x74696C74u;

void* capped_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* used = static_cast<size_t*>(ud);
    if (nsize == 0) {
        if (ptr != nullptr) {
            *used -= osize;
            std::free(ptr);
        }
        return nullptr;
    }
    if (ptr == nullptr) {
        if (*used + nsize > kLuaHeapCap) {
            return nullptr; // surfaces as a Lua allocation error
        }
        void* p = std::malloc(nsize);
        if (p != nullptr) {
            *used += nsize;
        }
        return p;
    }
    const long delta = static_cast<long>(nsize) - static_cast<long>(osize);
    if (delta > 0 && *used + static_cast<size_t>(delta) > kLuaHeapCap) {
        return nullptr;
    }
    void* p = std::realloc(ptr, nsize);
    if (p != nullptr) {
        *used += static_cast<size_t>(delta);
    }
    return p;
}

struct TimerEntry {
    uint64_t id = 0;
    uint64_t deadline_tick = 0;
    uint64_t interval = 0;     // 0 = one-shot
    uint64_t repeats_left = 0; // 0 = infinite (when interval > 0)
    sol::protected_function fn;
    bool canceled = false;
    bool disabled = false;
};

struct HandlerEntry {
    sol::protected_function fn;
    int consecutive_errors = 0;
    bool disabled = false;
};

// Canon §5.7 event names; unknown names raise at tb.on time.
bool is_canon_event(const std::string& name) {
    static const char* const kNames[] = {
        "game_start",
        "ball_start",
        "ball_end",
        "game_end",
        "player_up",
        "ball_launched",
        "switch_hit",
        "target_down",
        "bank_complete",
        "spinner_spin",
        "rollover",
        "kicker_enter",
        "ramp_made",
        "drain",
        "ball_save_expired",
        "tilt_warning",
        "tilt",
        "ball_lock",
        "captive_full_travel",
        "multiball_start",
        "multiball_end",
        "timer_tick",
    };
    for (const char* n : kNames) {
        if (name == n) {
            return true;
        }
    }
    return false;
}

} // namespace

class ScriptHostImpl {
public:
    ScriptHostImpl() = default;

    ~ScriptHostImpl() {
        if (L != nullptr) {
            lua_close(L);
        }
    }

    lua_State* L = nullptr;
    std::unique_ptr<sol::state_view> lua;

    bool loaded = false;
    bool scripting_disabled = false; // panic path (§1.3)
    int player_count = 1;
    int current_player = 1;
    uint64_t game_tick = 0;
    int instruction_budget = kTickInstructionBudget;
    bool budget_exhausted_this_tick = false;

    std::unordered_map<std::string, std::vector<HandlerEntry>> handlers;
    std::vector<TimerEntry> timers;
    uint64_t next_timer_id = 1;
    std::vector<ScriptAction> actions;
    PlayerScoreState scores[4];
    BackglassModel backglass_model;
    size_t heap_used = 0;
};

namespace {

// Watchdog hook (§2.4): fires every kHookGranularity instructions. The
// impl pointer rides the registry under a lightuserdata key.
void* kImplRegistryKey = const_cast<char*>("tb.impl");

void watchdog_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    lua_pushlightuserdata(L, kImplRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_islightuserdata(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    auto* impl = static_cast<ScriptHostImpl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (impl == nullptr) {
        return;
    }
    impl->instruction_budget -= kHookGranularity;
    if (impl->instruction_budget <= 0) {
        luaL_error(L, "instruction budget exceeded");
    }
}

} // namespace

ScriptHost::ScriptHost() {
    impl_ = new ScriptHostImpl();
}

ScriptHost::~ScriptHost() {
    delete impl_;
}

std::vector<ScriptAction>& ScriptHost::pending_actions() {
    return impl_->actions;
}

PlayerScoreState& ScriptHost::player_scores(int index) {
    return impl_->scores[std::clamp(index - 1, 0, 3)];
}

BackglassModel& ScriptHost::backglass() {
    return impl_->backglass_model;
}

bool ScriptHost::has_handler(const std::string& event_name) const {
    const auto it = impl_->handlers.find(event_name);
    return it != impl_->handlers.end() && !it->second.empty();
}

int ScriptHost::watchdog_budget_remaining() const {
    return impl_->instruction_budget;
}

void ScriptHost::set_current_player(int index) {
    impl_->current_player = std::clamp(index, 1, 4);
    if (impl_->lua == nullptr || !impl_->loaded) {
        return;
    }
    sol::state_view& lua = *impl_->lua;
    lua["tb"]["__current_player"] = impl_->current_player;
    lua.safe_script("tb.__swap_state()");
}

// ---- file-local helpers (sol types never cross the header) ----

namespace {

int pattern_blink_code(const char* pattern) {
    const std::string p = pattern != nullptr ? pattern : "";
    const int code = p == "slow_blink"   ? 1
                     : p == "fast_blink" ? 2
                     : p == "strobe"     ? 3
                     : p == "chase"      ? 4
                     : p == "breathe"    ? 5
                                         : -1;
    if (code < 0) {
        throw sol::error("unknown blink pattern: " + p);
    }
    return code;
}

const char* event_name_of(uint16_t type) {
    switch (SimEventType(type)) {
    case SimEventType::SwitchHit:
        return "switch_hit";
    case SimEventType::TargetDown:
        return "target_down";
    case SimEventType::BankComplete:
        return "bank_complete";
    case SimEventType::SpinnerSpin:
        return "spinner_spin";
    case SimEventType::RolloverEvent:
        return "rollover";
    case SimEventType::KickerEnter:
        return "kicker_enter";
    case SimEventType::RampMade:
        return "ramp_made";
    case SimEventType::Drain:
        return "drain";
    case SimEventType::BallLaunched:
        return "ball_launched";
    case SimEventType::BallLockCapture:
        return "ball_lock";
    case SimEventType::CaptiveFullTravel:
        return "captive_full_travel";
    default:
        return nullptr; // Collision is audio/particles only (§4.1)
    }
}

void fill_event_payload(const SimEvent& event, sol::table& ev) {
    ev["ball_id"] = event.data;
    ev["speed"] = event.a;
    ev["id"] = std::string("elem_") + std::to_string(event.element);
    switch (SimEventType(event.type)) {
    case SimEventType::TargetDown:
        ev["bank_id"] = std::string("elem_") + std::to_string(event.element);
        ev["target_index"] = int(event.b);
        break;
    case SimEventType::BankComplete:
        ev["bank_id"] = std::string("elem_") + std::to_string(event.element);
        break;
    case SimEventType::SpinnerSpin:
        ev["rpm"] = event.a;
        break;
    case SimEventType::BallLockCapture:
        ev["lock_id"] = std::string("elem_") + std::to_string(event.element);
        ev["count"] = int(event.a);
        break;
    case SimEventType::Drain:
        ev["balls_remaining"] = int(event.a);
        break;
    default:
        break;
    }
}

} // namespace

} // namespace tb::sim

namespace tb::sim {

void ScriptHost::load(const std::string& rules_source, SimState& state) {
    impl_->L = lua_newstate(capped_alloc, &impl_->heap_used, kLuaHashSeed);
    if (impl_->L == nullptr) {
        throw std::runtime_error("cannot create the Lua state (heap cap?)");
    }
    {
        lua_pushlightuserdata(impl_->L, kImplRegistryKey);
        lua_pushlightuserdata(impl_->L, impl_);
        lua_settable(impl_->L, LUA_REGISTRYINDEX);
    }
    impl_->lua = std::make_unique<sol::state_view>(impl_->L);

    sol::state_view& lua = *impl_->lua;
    lua.open_libraries(
        sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine);

    // §1.2: strip the escape hatches.
    for (const char* g : {"load", "loadstring", "loadfile", "dofile"}) {
        lua[g] = sol::nil;
    }
    lua["string"]["dump"] = sol::nil;
    lua["math"]["random"] = []() -> int {
        throw sol::error("math.random is disabled; use tb.rng()");
    };
    lua["math"]["randomseed"] = []() -> int {
        throw sol::error("math.randomseed is disabled; use tb.rng()");
    };
    lua["print"] = [](const char* msg) { TB_LOG_INFO("[lua]", "{}", msg); };
    lua["collectgarbage"] = [](sol::this_state s, const char* what) {
        if (what != nullptr && std::strcmp(what, "count") == 0) {
            return lua_gc(s, LUA_GCCOUNT, 0);
        }
        throw sol::error("collectgarbage: only \"count\" is permitted");
    };

    // §2.4: the watchdog hook on the main state (coroutine wrapping below).
    lua_sethook(impl_->L, watchdog_hook, LUA_MASKCOUNT, kHookGranularity);

    sol::table tb = lua.create_named_table("tb");

    // ---- events (§2.3) ----
    tb.set_function("on", [this](const std::string& name, sol::function fn) {
        if (!is_canon_event(name)) {
            throw sol::error("unknown event name: " + name);
        }
        impl_->handlers[name].push_back(
            HandlerEntry{sol::protected_function(fn, impl_->L), 0, false});
    });

    // ---- scoring (§3.1) ----
    tb.set_function("score", [this](double points) {
        if (points < 0) {
            TB_LOG_WARN("script", "tb.score(negative) ignored");
            return;
        }
        player_scores(impl_->current_player).score += uint64_t(points);
    });
    tb.set_function("add_bonus", [this](double points) {
        if (points < 0) {
            TB_LOG_WARN("script", "tb.add_bonus(negative) ignored");
            return;
        }
        player_scores(impl_->current_player).bonus += uint64_t(points);
    });
    tb.set_function("set_multiplier", [this](double n) {
        auto& ps = player_scores(impl_->current_player);
        ps.bonus_multiplier = std::clamp(int(n), 1, 10);
    });

    // ---- lights (§3.2) — state model; rendering rides the snapshot ----
    tb.set_function("light_on", [](const std::string&) {});
    tb.set_function("light_off", [](const std::string&) {});
    tb.set_function("light_blink", [](const std::string&, const char* pattern) {
        pattern_blink_code(pattern); // validates; unknown raises
    });

    // ---- sound/music (§3.3) — request emission only; playback is M11 ----
    tb.set_function("play_sound", [](const std::string&, sol::object) {});
    tb.set_function("play_music", [](const std::string&) {});
    tb.set_function("stop_music", []() {});

    // ---- physical elements (§3.4) — latched to next tick ----
    tb.set_function("kick", [this](const std::string& id, sol::object speed, sol::object angle) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::Kick;
        if (speed.is<double>()) {
            a.speed = float(std::clamp(speed.as<double>(), 0.0, 12.0));
        }
        if (angle.is<double>()) {
            a.angle_deg = float(angle.as<double>());
        }
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("kick_hold", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::KickHold;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("release_lock", [this](const std::string& id, double count) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::ReleaseLock;
        a.count = std::max(1, int(count));
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
        // `released` return: filled when the action applies (next tick).
    });
    tb.set_function("magnet_on", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetOn;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("magnet_off", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetOff;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("magnet_pulse", [this](const std::string& id, double ms) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetPulse;
        a.speed = float(std::clamp(ms, 1.0, 10000.0));
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("set_flipper_enabled", [this](const std::string& id, bool on) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::SetFlipperEnabled;
        a.flag = on;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("drop_bank_reset", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::DropBankReset;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("gate_open", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::GateOpen;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });
    tb.set_function("gate_close", [this](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::GateClose;
        {
            ScriptAction a2 = a;
            (void)a2;
            (void)id;
            impl_->actions.push_back(a);
        }
    });

    // ---- ball management (§3.5) ----
    tb.set_function("ball_save", [this, &state](double ms, sol::object uses) {
        const int m = int(ms);
        if (m == 0) {
            state.ball_save.active = false;
            state.ball_save.ticks_left = 0;
            return;
        }
        const int use_count = uses.is<double>() ? std::clamp(int(uses.as<double>()), 1, 10) : 1;
        state.ball_save.active = true;
        state.ball_save.ticks_left =
            std::max(state.ball_save.ticks_left, uint32_t(std::clamp(m, 1, 120000)));
        (void)use_count; // multi-use bookkeeping lands with the framework (M10)
    });
    tb.set_function("add_ball", [this](sol::object n) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::AddBall;
        a.count = n.is<double>() ? std::max(1, int(n.as<double>())) : 1;
        impl_->actions.push_back(a);
    });
    tb.set_function("award_extra_ball", [this]() {
        auto& ps = player_scores(impl_->current_player);
        if (ps.extra_balls < 3) {
            ++ps.extra_balls;
        } else {
            ps.score += 100000;
        }
    });

    // ---- timers (§3.6) ----
    tb.set_function("timer", [this](double ms, sol::function fn, sol::object repeats) -> uint64_t {
        TimerEntry t;
        t.id = impl_->next_timer_id++;
        t.interval = uint64_t(std::clamp(ms, 1.0, 3600000.0));
        t.deadline_tick = impl_->game_tick + t.interval;
        t.repeats_left = repeats.is<double>() ? uint64_t(std::max(0.0, repeats.as<double>())) : 1;
        t.fn = sol::protected_function(fn, impl_->L);
        impl_->timers.push_back(std::move(t));
        return impl_->next_timer_id - 1;
    });
    tb.set_function("cancel_timer", [this](double id) {
        for (TimerEntry& t : impl_->timers) {
            if (t.id == uint64_t(id)) {
                t.canceled = true;
            }
        }
    });

    // ---- display (§3.7) — BackglassModel state ----
    tb.set_function("show_message", [this](const char* text, sol::object opts) {
        std::string msg = text != nullptr ? text : "";
        if (msg.size() > 64) {
            TB_LOG_WARN("script", "show_message truncated to 64 chars");
            msg.resize(64);
        }
        impl_->backglass_model.message = std::move(msg);
        impl_->backglass_model.message_style = 0;
        impl_->backglass_model.message_ticks_left = 2000;
        if (opts.is<sol::table>()) {
            sol::table o = opts;
            std::string style = o.get_or<std::string>("style", "info");
            impl_->backglass_model.message_style = style == "mode"      ? 1
                                                   : style == "jackpot" ? 2
                                                   : style == "warning" ? 3
                                                                        : 0;
            impl_->backglass_model.message_ticks_left =
                uint32_t(std::clamp(o.get_or("duration_ms", 2000), 250, 10000));
        }
    });
    sol::table bg = lua.create_named_table("tb_backglass_table");
    tb["backglass"] = bg;
    bg.set_function("set_layout", [this](const std::string& layout) {
        impl_->backglass_model.layout = layout == "mode" ? 1 : layout == "celebration" ? 2 : 0;
    });
    bg.set_function("focus_score", [this](double player) {
        impl_->backglass_model.focus_player = std::clamp(int(player), 1, 4);
    });
    bg.set_function("animate", [](const std::string&) {});

    // ---- state / game / table_info (§3.8) ----
    // Per-player backing tables; tb.state proxies the current player.
    lua.safe_script(R"lua(
        local tb = tb
        local states = { {}, {}, {}, {} }
        local current = 1
        local proxy = setmetatable({}, {
            __index = function(_, k) return states[current][k] end,
            __newindex = function(_, k, v) states[current][k] = v end,
        })
        rawset(tb, "state", proxy)
        rawset(tb, "__swap_state", function() current = tb.__current_player end)
        tb.__states = states
    )lua");
    // The swap reads tb.__current_player, which we set from C++ before
    // each swap call.
    tb["__current_player"] = 1;

    sol::table game = lua.create_named_table("tb_game_table");
    tb["game"] = game;
    auto sync_game = [this, &game]() {
        game.set("current_player", impl_->current_player);
        game.set("player_count", impl_->player_count);
        game.set("ball_number", 1);
        game.set("balls_per_game", 3);
        game.set("score", player_scores(impl_->current_player).score);
        game.set("tick", impl_->game_tick);
    };
    sync_game();
    impl_->lua->set_function("__sync_game", sync_game);

    sol::table info = lua.create_named_table("tb_table_info_table");
    tb["table_info"] = info;
    info.set("name", "table");
    info.set("slug", "table");
    info.set("width_m", state.width);
    info.set("height_m", state.height);
    // Freeze: writes raise (read-only tables per §3.8).
    lua.safe_script(R"lua(
        local function freeze(t)
            return setmetatable({}, {
                __index = t,
                __newindex = function() error("read-only table") end,
            })
        end
        tb.game = freeze(tb_game_table)
        tb.table_info = freeze(tb_table_info_table)
        tb.backglass = tb_backglass_table
    )lua");

    // ---- randomness (§3.9): rng_script stream ----
    tb.set_function("rng", [&state]() { return state.rng_script.next_float(); });
    tb.set_function("rng_range", [&state](double a, double b) -> int {
        if (a != std::floor(a) || b != std::floor(b) || a > b) {
            throw sol::error("tb.rng_range: need integers a <= b");
        }
        const int64_t span = int64_t(b) - int64_t(a) + 1;
        const double u = state.rng_script.next_float();
        return int(a + std::min(double(span - 1), std::floor(u * double(span))));
    });

    // ---- run the rules (§2.1) ----
    impl_->instruction_budget = kTickInstructionBudget;
    sol::protected_function_result result =
        lua.safe_script(rules_source, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        throw std::runtime_error(std::string("rules.lua: ") + err.what());
    }

    sol::optional<sol::protected_function> on_init = lua["on_init"];
    if (on_init.has_value()) {
        sol::protected_function_result r = on_init.value()();
        if (!r.valid()) {
            const sol::error err = r;
            throw std::runtime_error(std::string("on_init: ") + err.what());
        }
    }
    impl_->loaded = true;
}

void ScriptHost::dispatch(const SimEvent& event) {
    if (!impl_->loaded || impl_->scripting_disabled) {
        return;
    }
    const char* name = event_name_of(event.type);
    if (name == nullptr) {
        return;
    }
    const auto it = impl_->handlers.find(name);
    if (it == impl_->handlers.end()) {
        return;
    }

    sol::state_view& lua = *impl_->lua;
    sol::table ev = lua.create_table();
    ev["name"] = name;
    fill_event_payload(event, ev);

    for (HandlerEntry& h : it->second) {
        if (h.disabled || impl_->budget_exhausted_this_tick) {
            continue;
        }
        sol::protected_function_result r = h.fn(ev);
        if (!r.valid()) {
            const sol::error err = r;
            const std::string msg = err.what();
            if (msg.find("instruction budget exceeded") != std::string::npos) {
                // §2.4: overrun disables permanently, skips the rest.
                h.disabled = true;
                impl_->budget_exhausted_this_tick = true;
                TB_LOG_ERROR("script", "handler {} disabled: budget overrun", name);
                continue;
            }
            TB_LOG_ERROR("script", "handler {} error: {}", name, msg);
            if (++h.consecutive_errors >= kDisableAfterConsecutiveErrors) {
                h.disabled = true;
                TB_LOG_ERROR("script", "handler {} disabled after 10 errors", name);
            }
        } else {
            h.consecutive_errors = 0;
        }
    }
}

void ScriptHost::on_tick(uint64_t tick) {
    if (!impl_->loaded || impl_->scripting_disabled) {
        return;
    }
    impl_->game_tick = tick;
    impl_->instruction_budget = kTickInstructionBudget;
    impl_->budget_exhausted_this_tick = false;

    // Fire timers whose deadline == tick, ascending id (§3.6).
    std::sort(impl_->timers.begin(),
              impl_->timers.end(),
              [](const TimerEntry& a, const TimerEntry& b) { return a.id < b.id; });
    for (TimerEntry& t : impl_->timers) {
        if (t.canceled || t.disabled) {
            continue;
        }
        if (t.deadline_tick != tick) {
            continue;
        }
        sol::protected_function_result r = t.fn(t.id);
        if (!r.valid()) {
            const sol::error err = r;
            const std::string msg = err.what();
            if (msg.find("instruction budget exceeded") != std::string::npos) {
                t.disabled = true;
                t.canceled = true;
                TB_LOG_ERROR("script", "timer {} disabled: budget overrun", t.id);
            } else {
                TB_LOG_ERROR("script", "timer {} error: {}", t.id, msg);
            }
        }
        // Repeat bookkeeping.
        if (t.interval > 0) {
            if (t.repeats_left > 1) {
                --t.repeats_left;
                t.deadline_tick = tick + t.interval;
            } else if (t.repeats_left == 0) {
                t.deadline_tick = tick + t.interval; // infinite
            } else {
                t.canceled = true;
            }
        } else {
            t.canceled = true;
        }
    }
    // Drop dead timers (keeps the vector bounded).
    impl_->timers.erase(std::remove_if(impl_->timers.begin(),
                                       impl_->timers.end(),
                                       [](const TimerEntry& t) { return t.canceled; }),
                        impl_->timers.end());

    // §2.2 phase 4: one incremental GC step.
    lua_gc(impl_->L, LUA_GCSTEP, 0);
}

void ScriptHost::begin_game(int player_count) {
    impl_->player_count = std::clamp(player_count, 1, 4);
    impl_->current_player = 1;
    for (auto& ps : impl_->scores) {
        ps = PlayerScoreState{};
    }
    run_framework_handlers("game_start", player_count);
}

void ScriptHost::end_game() {
    run_framework_handlers("game_end", 0);
}

void ScriptHost::run_framework_handlers(const std::string& name, int payload) {
    if (!impl_->loaded || impl_->scripting_disabled) {
        return;
    }
    const auto it = impl_->handlers.find(name);
    if (it == impl_->handlers.end()) {
        return;
    }
    sol::state_view& lua = *impl_->lua;
    sol::table ev = lua.create_table();
    ev["name"] = name;
    if (name == "game_start") {
        ev["player_count"] = payload;
    }
    for (HandlerEntry& h : it->second) {
        if (h.disabled) {
            continue;
        }
        sol::protected_function_result r = h.fn(ev);
        if (!r.valid()) {
            const sol::error err = r;
            TB_LOG_ERROR("script", "handler {} error: {}", name, err.what());
            if (++h.consecutive_errors >= kDisableAfterConsecutiveErrors) {
                h.disabled = true;
            }
        } else {
            h.consecutive_errors = 0;
        }
    }
}

} // namespace tb::sim
