#include "sim/script_host.h"

#include "core/log.h"
#include "sim/solver.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

// Sandbox + tick pipeline implementation (10-scripting.md §1–§4). One
// lua_State per running game, sim-thread only.
//
// Layout: constants → handler/timer entries → ScriptHostImpl → anonymous
// helper namespace → ScriptHost methods. Sol types never cross the header.
namespace tb::sim {

// The impl pointer for the watchdog hook. Debug hooks run during arbitrary
// VM execution where free Lua stack slots are NOT guaranteed, so the hook
// must not push anything — it reads this thread-local pointer instead.
// Safe by the §1.1 invariant: exactly one live host (and one lua_State)
// per thread at a time; load() sets it before any hook can fire and the
// destructor clears it after lua_close.
thread_local ScriptHostImpl* t_active_impl = nullptr;

// §1.1: 64 MiB Lua heap cap.
constexpr size_t kLuaHeapCap = 64u * 1024u * 1024u;
// §2.4: 10,000 instructions per tick at 1,000-instruction hook granularity.
constexpr int kTickInstructionBudget = 10000;
constexpr int kHookGranularity = 1000;
// §2.5: 10 consecutive failures disable a handler.
constexpr int kDisableAfterConsecutiveErrors = 10;
// §3.7: message length cap.
constexpr uint32_t kMessageMaxLen = 64;

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
    if (nsize > osize && *used + (nsize - osize) > kLuaHeapCap) {
        return nullptr;
    }
    void* p = std::realloc(ptr, nsize);
    if (p != nullptr) {
        *used = *used + nsize - osize;
    }
    return p;
}

struct HandlerEntry {
    sol::protected_function fn;
    int consecutive_errors = 0;
    bool disabled = false;
};

struct TimerEntry {
    uint64_t id = 0;
    uint64_t deadline_tick = 0;
    uint64_t interval = 0;     // 0 = one-shot
    uint64_t repeats_left = 0; // 0 = infinite (when interval > 0)
    sol::protected_function fn;
    bool canceled = false;
    bool disabled = false;
};

struct ScriptHostImpl {
    lua_State* L = nullptr;
    std::unique_ptr<sol::state_view> lua;

    SimState* sim = nullptr; // non-owning; set at load
    bool loaded = false;
    bool scripting_disabled = false; // panic path (§1.3)
    int player_count = 1;
    int current_player = 1;
    int ball_number = 1; // framework-owned (M10); default for tb.game
    int balls_per_game = 3;
    uint64_t game_tick = 0;
    int instruction_budget = kTickInstructionBudget;
    bool budget_exhausted_this_tick = false;
    bool timers_frozen = false; // §3.6

    std::unordered_map<std::string, std::vector<HandlerEntry>> handlers;
    std::vector<TimerEntry> timers;
    uint64_t next_timer_id = 1;
    std::vector<ScriptAction> actions;
    PlayerScoreState scores[4];
    BackglassModel backglass_model;
    size_t heap_used = 0;

    // Static build data copied at load: id → TableDef element index.
    std::unordered_map<std::string, uint16_t> element_index;
    std::vector<std::string> element_ids;
    std::vector<std::vector<std::string>> element_tags;

    ~ScriptHostImpl() {
        if (L != nullptr) {
            // Member sol objects (handlers/timers/state_view) unref from
            // the state in their destructors — which would run AFTER the
            // body's lua_close on a freed state (the arm64-macOS and
            // Linux/5.4 segfault; x86 5.5 survived by heap luck). Release
            // every sol-holding member manually BEFORE the close.
            handlers.clear();
            timers.clear();
            lua.reset();
            lua_sethook(L, nullptr, 0, 0); // no hooks during teardown
            lua_close(L);
            if (t_active_impl == this) {
                t_active_impl = nullptr;
            }
        }
    }
};

namespace {

ScriptHostImpl* impl_from_state(lua_State*) {
    return t_active_impl;
}

// §2.4 watchdog: fires every kHookGranularity instructions, decrements the
// shared tick budget, and raises once exhausted.
void watchdog_hook(lua_State* L, lua_Debug*) {
    ScriptHostImpl* impl = impl_from_state(L);
    if (impl == nullptr) {
        return;
    }
    impl->instruction_budget -= kHookGranularity;
    if (impl->instruction_budget <= 0) {
        // Flag at raise time: the error is catchable (pcall/resume), so
        // run_handler may never see it — later invocations this tick must
        // still be skipped (§2.4), not disabled.
        impl->budget_exhausted_this_tick = true;
        luaL_error(L, "instruction budget exceeded");
    }
}

// §1.2: coroutine.create must produce threads that carry the watchdog
// hook (Lua 5.4 does not inherit it). Mirrors lcorolib's create.
int hooked_co_create(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* co = lua_newthread(L);
    if (co == nullptr) {
        return luaL_error(L, "cannot create coroutine");
    }
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    lua_sethook(co, watchdog_hook, LUA_MASKCOUNT, kHookGranularity);
    return 1;
}

// Budget-error rethrow: a raw lua_CFunction on purpose — luaL_error
// longjmps past C++ destructors, so an erroring binding must hold no
// std::string/std::object in scope (LeakSanitizer-visible otherwise).
int rethrow_budget(lua_State* L) {
    size_t len = 0;
    const char* msg = lua_tolstring(L, 1, &len); // view, no allocation
    static const char kBudgetError[] = "instruction budget exceeded";
    const size_t needle = sizeof(kBudgetError) - 1;
    if (msg != nullptr && len >= needle &&
        std::search(msg, msg + len, kBudgetError, kBudgetError + needle) != msg + len) {
        return luaL_error(L, "instruction budget exceeded");
    }
    return lua_error(L); // ordinary error: re-raise the original value
}

// Canon §5.7 event names (payloads per 10-scripting.md §4).
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

// Whether the event type's payload carries a ball id (§4).
bool event_carries_ball_id(uint16_t type) {
    switch (SimEventType(type)) {
    case SimEventType::SwitchHit:
    case SimEventType::RolloverEvent:
    case SimEventType::KickerEnter:
    case SimEventType::RampMade:
    case SimEventType::Drain:
    case SimEventType::BallLaunched:
    case SimEventType::TargetDown:
        return true;
    default:
        return false;
    }
}

int pattern_blink_code(const char* pattern, lua_State* L) {
    const std::string p = pattern != nullptr ? pattern : "";
    const int code = p == "slow_blink"   ? 1
                     : p == "fast_blink" ? 2
                     : p == "strobe"     ? 3
                     : p == "chase"      ? 4
                     : p == "breathe"    ? 5
                                         : -1;
    if (code < 0) {
        luaL_error(L, "unknown blink pattern: %s", p.c_str());
    }
    return code;
}

void fill_event_payload(ScriptHostImpl& impl, const SimEvent& event, sol::table& ev) {
    const std::string& id =
        event.element < impl.element_ids.size() ? impl.element_ids[event.element] : std::string();
    ev.set("id", id);
    if (event_carries_ball_id(event.type)) {
        ev.set("ball_id", int64_t(event.data) + 1); // 1-based (§4)
    }
    ev.set("speed", double(event.a));
    switch (SimEventType(event.type)) {
    case SimEventType::TargetDown:
        ev.set("bank_id", id);
        ev.set("target_index", int64_t(event.b));
        break;
    case SimEventType::BankComplete:
        ev.set("bank_id", id);
        break;
    case SimEventType::SpinnerSpin:
        ev.set("rpm", double(event.a));
        break;
    case SimEventType::BallLockCapture:
        ev.set("lock_id", id);
        ev.set("count", int64_t(event.a));
        break;
    case SimEventType::Drain:
        ev.set("balls_remaining", int64_t(event.a));
        break;
    default:
        break;
    }
    // tags: prebuilt per element at load (tb.__tags), so dispatch never
    // builds a fresh Lua table.
    sol::state_view lua(impl.L);
    sol::object tags = lua["tb"]["__tags"][uint64_t(event.element) + 1];
    ev.set("tags", tags.is<sol::table>() ? tags : sol::lua_nil);
}

// One handler invocation under the §2.5 error policy. Returns true when
// the handler stays enabled.
bool run_handler(ScriptHostImpl& impl,
                 HandlerEntry& h,
                 const sol::table& ev,
                 const std::string& name) {
    if (impl.budget_exhausted_this_tick) {
        return true; // §2.4: skip, but do not disable, later invocations
    }
    sol::protected_function_result r = h.fn(ev);
    if (r.valid()) {
        h.consecutive_errors = 0;
        return true;
    }
    const sol::error err = r;
    const std::string msg = err.what();
    if (msg.find("instruction budget exceeded") != std::string::npos) {
        TB_LOG_ERROR("script", "handler {} disabled: budget overrun", name);
        impl.budget_exhausted_this_tick = true;
        return false; // permanent disable (§2.4)
    }
    TB_LOG_ERROR("script", "handler {} error: {}", name, msg);
    return ++h.consecutive_errors < kDisableAfterConsecutiveErrors;
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

bool ScriptHost::scripting_active() const {
    return impl_->loaded && !impl_->scripting_disabled;
}

bool ScriptHost::state_read_int(int player, const char* key, int64_t& out) const {
    if (impl_->lua == nullptr || !impl_->loaded) {
        return false;
    }
    sol::state_view lua(*impl_->lua);
    sol::object v = lua["tb"]["__states"][std::clamp(player, 1, 4)][key];
    if (v.is<int64_t>()) {
        out = v.as<int64_t>();
        return true;
    }
    if (v.is<double>()) {
        out = int64_t(v.as<double>());
        return true;
    }
    if (v.is<bool>()) {
        out = v.as<bool>() ? 1 : 0;
        return true;
    }
    return false;
}

void ScriptHost::set_current_player(int index) {
    impl_->current_player = std::clamp(index, 1, 4);
    if (impl_->lua == nullptr || !impl_->loaded) {
        return;
    }
    sol::state_view lua(*impl_->lua);
    lua["tb"]["__current_player"] = impl_->current_player;
    auto swap = lua["tb"]["__swap_state"];
    if (swap.valid()) {
        sol::protected_function_result r = swap();
        if (!r.valid()) {
            TB_LOG_ERROR("script", "tb.state swap failed");
        }
    }
}

void ScriptHost::set_timers_frozen(bool frozen) {
    impl_->timers_frozen = frozen;
}

void ScriptHost::load(const std::string& rules_source, SimState& state) {
    if (impl_->L != nullptr) {
        // A second load would leak the lua_State and keep stale handlers
        // / element ids; hot reload constructs a fresh host instead.
        throw std::runtime_error("ScriptHost::load called twice; construct a new ScriptHost");
    }
    impl_->sim = &state;
    impl_->element_ids = state.element_ids;
    impl_->element_tags = state.element_tags;
    for (uint16_t i = 0; i < state.element_ids.size(); ++i) {
        impl_->element_index[state.element_ids[i]] = i;
    }

    // 5.5.1 exposes the seeded form; the fixed seed keeps §1.4's intent.
    impl_->L = lua_newstate(capped_alloc, &impl_->heap_used, 0x74696C74u);
    if (impl_->L == nullptr) {
        throw std::runtime_error("cannot create the Lua state (heap cap?)");
    }
    t_active_impl = impl_;
    impl_->lua = std::make_unique<sol::state_view>(impl_->L);

    sol::state_view& lua = *impl_->lua;
    lua.open_libraries(
        sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine);

    // §1.2 whitelist: strip the escape hatches.
    for (const char* g : {"load", "loadstring", "loadfile", "dofile"}) {
        lua[g] = sol::lua_nil;
    }
    lua["string"]["dump"] = sol::lua_nil;
    // luaL_error is NOT noreturn in the Lua headers: the returns are
    // dead at runtime (it longjmps) but keep the lambdas well-formed —
    // falling off a non-void function is UB that -O2 punishes.
    lua["math"]["random"] = [](sol::this_state s) -> int {
        luaL_error(s, "math.random is disabled; use tb.rng()");
        return 0;
    };
    lua["math"]["randomseed"] = [](sol::this_state s) -> int {
        luaL_error(s, "math.randomseed is disabled; use tb.rng()");
        return 0;
    };
    lua["print"] = [](const char* msg) { TB_LOG_INFO("[lua]", "{}", msg); };
    lua["collectgarbage"] = [](sol::this_state s, const char* what) {
        if (what != nullptr && std::strcmp(what, "count") == 0) {
            return lua_gc(s, LUA_GCCOUNT, 0);
        }
        luaL_error(s, "collectgarbage: only \"count\" is permitted");
        return 0;
    };

    // §2.4: the watchdog hook on the main state + hooked coroutine.create
    // (§1.2); coroutine.wrap is redefined in Lua below. Budget errors are
    // ordinary Lua errors and therefore catchable — pcall/xpcall are
    // shimmed to re-raise them so a `while true do pcall(...) end`
    // handler cannot outlive its budget (§2.4 breach).
    lua_sethook(impl_->L, watchdog_hook, LUA_MASKCOUNT, kHookGranularity);
    lua["coroutine"]["create"] = hooked_co_create;
    lua["__rethrow_budget"] = rethrow_budget;

    // pcall/xpcall shims: pass ordinary errors through, re-raise budget
    // errors uncatchably relative to script code.
    lua.safe_script(R"lua(
        local rethrow = __rethrow_budget
        local raw_pcall = pcall
        rawset(_G, "pcall", function(f, ...)
            local results = table.pack(raw_pcall(f, ...))
            if not results[1] and type(results[2]) == "string"
               and string.find(results[2], "instruction budget exceeded", 1, true) then
                return rethrow(results[2])
            end
            return table.unpack(results, 1, results.n)
        end)
        local raw_xpcall = xpcall
        rawset(_G, "xpcall", function(f, handler, ...)
            local ok, err = raw_xpcall(f, handler, ...)
            if not ok and type(err) == "string"
               and string.find(err, "instruction budget exceeded", 1, true) then
                return rethrow(err)
            end
            return ok, err
        end)
    )lua");

    sol::table tb = lua.create_named_table("tb");

    // ---- events (§2.3) ----
    tb.set_function("on", [this](sol::this_state s, const std::string& name, sol::function fn) {
        if (!is_canon_event(name)) {
            luaL_error(s, "unknown event name: %s", name.c_str());
        }
        impl_->handlers[name].push_back(HandlerEntry{sol::protected_function(fn), 0, false});
    });

    // ---- scoring (§3.1) ----
    tb.set_function("score", [this](double points) {
        if (!(points >= 0.0) || points > double(std::numeric_limits<uint64_t>::max())) {
            // NaN fails the first comparison; the second bounds the cast.
            TB_LOG_WARN("script", "tb.score(negative/NaN) ignored");
            return;
        }
        player_scores(impl_->current_player).score += uint64_t(points);
    });
    tb.set_function("add_bonus", [this](double points) {
        if (!(points >= 0.0) || points > double(std::numeric_limits<uint64_t>::max())) {
            TB_LOG_WARN("script", "tb.add_bonus(negative/NaN) ignored");
            return;
        }
        player_scores(impl_->current_player).bonus += uint64_t(points);
    });
    tb.set_function("set_multiplier", [this](double n) {
        const int clamped = int(std::clamp(std::isfinite(n) ? n : 1.0, 1.0, 10.0));
        player_scores(impl_->current_player).bonus_multiplier = clamped;
    });

    // ---- lights (§3.2): flip SimState light state by element id ----
    auto set_light = [this](const std::string& id, bool on) {
        const auto it = impl_->element_index.find(id);
        if (it == impl_->element_index.end()) {
            TB_LOG_WARN("script", "light: unknown id {}", id);
            return;
        }
        for (LightState& light : impl_->sim->lights) {
            if (light.table_id == it->second) {
                light.on = on;
            }
        }
    };
    tb.set_function("light_on", [set_light](const std::string& id) { set_light(id, true); });
    tb.set_function("light_off", [set_light](const std::string& id) { set_light(id, false); });
    tb.set_function("light_blink",
                    [set_light](sol::this_state s, const std::string& id, const char* pattern) {
                        pattern_blink_code(pattern, s); // validates; unknown raises
                        set_light(id, true);            // blink rendering is M13; state-wise on
                    });

    // ---- sound/music (§3.3): request emission only; playback is M11 ----
    tb.set_function("play_sound", [](const std::string&, sol::object) {});
    tb.set_function("play_music", [](const std::string&) {});
    tb.set_function("stop_music", []() {});

    // ---- physical elements (§3.4): latched to next tick ----
    auto latch = [this](ScriptAction a, const std::string& id) -> uint16_t {
        const auto it = impl_->element_index.find(id);
        if (it == impl_->element_index.end()) {
            TB_LOG_WARN("script", "unknown element id {}", id);
            return 0xFFFF;
        }
        a.element = it->second;
        impl_->actions.push_back(a);
        return it->second;
    };
    tb.set_function("kick", [latch](const std::string& id, sol::object speed, sol::object angle) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::Kick;
        if (speed.is<double>()) {
            a.use_speed = true;
            a.speed = float(std::clamp(speed.as<double>(), 0.0, 12.0));
        }
        if (angle.is<double>()) {
            a.use_angle = true;
            a.angle_deg = float(angle.as<double>());
        }
        latch(a, id);
    });
    tb.set_function("kick_hold", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::KickHold;
        latch(a, id);
    });
    tb.set_function("release_lock", [this, latch](const std::string& id, double count) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::ReleaseLock;
        a.count = int(std::clamp(count, 1.0, 6.0));
        const uint16_t elem = latch(a, id);
        if (elem == 0xFFFF) {
            return 0;
        }
        // §3.4: return the integer actually released (read-only peek).
        for (const BallLockElem& lock : impl_->sim->ball_locks) {
            if (lock.common.table_id == elem) {
                return std::min(a.count, lock.held);
            }
        }
        return 0;
    });
    tb.set_function("magnet_on", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetOn;
        latch(a, id);
    });
    tb.set_function("magnet_off", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetOff;
        latch(a, id);
    });
    tb.set_function("magnet_pulse", [latch](const std::string& id, double ms) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::MagnetPulse;
        a.speed = float(std::clamp(ms, 1.0, 10000.0));
        latch(a, id);
    });
    tb.set_function("set_flipper_enabled", [latch](const std::string& id, bool on) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::SetFlipperEnabled;
        a.flag = on;
        latch(a, id);
    });
    tb.set_function("drop_bank_reset", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::DropBankReset;
        latch(a, id);
    });
    tb.set_function("gate_open", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::GateOpen;
        latch(a, id);
    });
    tb.set_function("gate_close", [latch](const std::string& id) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::GateClose;
        latch(a, id);
    });

    // ---- ball management (§3.5) ----
    tb.set_function("ball_save", [this](double ms, sol::object uses) {
        BallSaveState& bs = impl_->sim->ball_save;
        const int m = int(std::clamp(std::isfinite(ms) ? ms : 0.0,
                                     double(std::numeric_limits<int>::min()),
                                     double(std::numeric_limits<int>::max())));
        if (m == 0) {
            bs.active = false;
            bs.ticks_left = 0;
            return;
        }
        bs.active = true;
        bs.ticks_left = std::max(bs.ticks_left, uint32_t(std::clamp(m, 1, 120000)));
        (void)uses; // multi-use bookkeeping arrives with the framework (M10)
    });
    tb.set_function("add_ball", [this](sol::object n) {
        ScriptAction a{};
        a.kind = ScriptAction::Kind::AddBall;
        a.count = n.is<double>() ? int(std::clamp(n.as<double>(), 1.0, double(kMaxBalls))) : 1;
        impl_->actions.push_back(a);
    });
    tb.set_function("award_extra_ball", [this]() {
        PlayerScoreState& ps = player_scores(impl_->current_player);
        if (ps.extra_balls < 3) {
            ++ps.extra_balls;
        } else {
            ps.score += 100000; // §3.5: past the cap posts points
        }
    });

    // ---- timers (§3.6) ----
    tb.set_function("timer", [this](double ms, sol::function fn, sol::object repeats) -> uint64_t {
        TimerEntry t;
        t.id = impl_->next_timer_id++;
        t.interval = uint64_t(std::clamp(ms, 1.0, 3600000.0));
        t.deadline_tick = impl_->game_tick + t.interval;
        t.repeats_left =
            repeats.is<double>() && repeats.as<double>() >= 1 ? uint64_t(repeats.as<double>()) : 1;
        t.fn = sol::protected_function(fn);
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

    // ---- display (§3.7): BackglassModel state, fixed buffers ----
    tb.set_function("show_message", [this](const char* text, sol::object opts) {
        BackglassModel& bg = impl_->backglass_model;
        uint32_t len = 0;
        if (text != nullptr) {
            len = uint32_t(std::strlen(text));
        }
        if (len > kMessageMaxLen) {
            TB_LOG_WARN("script", "show_message truncated to 64 chars");
            len = kMessageMaxLen;
        }
        std::memcpy(bg.message, text, len);
        bg.message[len] = '\0';
        bg.message_len = len;
        bg.message_style = 0;
        bg.message_ticks_left = 2000;
        if (opts.is<sol::table>()) {
            sol::table o = opts;
            const std::string style = o.get_or<std::string>("style", "info");
            bg.message_style = style == "mode"      ? 1
                               : style == "jackpot" ? 2
                               : style == "warning" ? 3
                                                    : 0;
            bg.message_ticks_left = uint32_t(std::clamp(o.get_or("duration_ms", 2000), 250, 10000));
        }
    });
    sol::table bg = lua.create_named_table("tb_backglass_table");
    tb.set("backglass", bg);
    bg.set_function("set_layout", [this](const std::string& layout) {
        impl_->backglass_model.layout = layout == "mode" ? 1 : layout == "celebration" ? 2 : 0;
    });
    bg.set_function("focus_score", [this](double player) {
        impl_->backglass_model.focus_player = std::clamp(int(player), 1, 4);
    });
    bg.set_function("animate", [](const std::string&) {});

    // ---- tb.state proxy + swap seam + coroutine.wrap (§1.2, §3.8) ----
    lua.safe_script(R"lua(
        local states = { {}, {}, {}, {} }
        local current = 1
        local proxy = setmetatable({}, {
            __index = function(_, k) return states[current][k] end,
            __newindex = function(_, k, v) states[current][k] = v end,
        })
        rawset(tb, "state", proxy)
        rawset(tb, "__states", states)
        rawset(tb, "__swap_state", function() current = tb.__current_player end)
        rawset(tb, "__current_player", 1)
        coroutine.wrap = function(f)
            local co = coroutine.create(f)
            return function(...)
                local r = table.pack(coroutine.resume(co, ...))
                if r[1] then return table.unpack(r, 2, r.n) end
                error(r[2], 0)
            end
        end
    )lua");

    // ---- tb.game: read-only, always fresh via a C closure (§3.8) ----
    lua.set_function("__game_get", [this](const char* key) -> sol::object {
        sol::state_view lv(impl_->L);
        if (std::strcmp(key, "current_player") == 0) {
            return sol::make_object(lv, impl_->current_player);
        }
        if (std::strcmp(key, "player_count") == 0) {
            return sol::make_object(lv, impl_->player_count);
        }
        if (std::strcmp(key, "ball_number") == 0) {
            return sol::make_object(lv, impl_->ball_number);
        }
        if (std::strcmp(key, "balls_per_game") == 0) {
            return sol::make_object(lv, impl_->balls_per_game);
        }
        if (std::strcmp(key, "score") == 0) {
            return sol::make_object(lv, player_scores(impl_->current_player).score);
        }
        if (std::strcmp(key, "tick") == 0) {
            return sol::make_object(lv, impl_->game_tick);
        }
        return sol::lua_nil;
    });
    lua.safe_script(R"lua(
        rawset(tb, "game", setmetatable({}, {
            __index = function(_, k) return __game_get(k) end,
            __newindex = function() error("tb.game is read-only") end,
        }))
        rawset(tb, "table_info", setmetatable({
            name = "table", slug = "table",
        }, { __newindex = function() error("tb.table_info is read-only") end }))
        rawset(tb, "__tags", setmetatable({}, { __index = function() return {} end }))
    )lua");

    // ---- randomness (§3.9): rng_script stream ----
    tb.set_function("rng", [this]() { return impl_->sim->rng_script.next_float(); });
    tb.set_function("rng_range", [this](sol::this_state s, double a, double b) -> int {
        // Bounding to int range first keeps every integer conversion
        // below well-defined (double > INT64_MAX casts are UB).
        constexpr double kIntMin = -2147483648.0;
        constexpr double kIntMax = 2147483647.0;
        if (!std::isfinite(a) || !std::isfinite(b) || a != std::floor(a) || b != std::floor(b) ||
            a > b || a < kIntMin || b > kIntMax) {
            luaL_error(s, "tb.rng_range: need finite integers a <= b within int range");
        }
        const int64_t span = int64_t(b) - int64_t(a) + 1;
        const double u = impl_->sim->rng_script.next_float();
        return int(int64_t(a) + std::min<double>(double(span - 1), std::floor(u * double(span))));
    });

    // ---- run the rules (§2.1) ----
    impl_->instruction_budget = kTickInstructionBudget;
    sol::protected_function_result result =
        lua.safe_script(rules_source, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        throw std::runtime_error(std::string("rules.lua: ") + err.what());
    }

    // Prebuild the per-element tag tables (dispatch reuses them).
    {
        sol::state_view lv(impl_->L);
        sol::table tags_root = lv["tb"]["__tags"];
        for (uint16_t i = 0; i < impl_->element_tags.size(); ++i) {
            sol::table arr = lv.create_table();
            for (const std::string& t : impl_->element_tags[i]) {
                arr.add(t);
            }
            tags_root[uint64_t(i) + 1] = arr;
        }
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

void ScriptHost::begin_tick(uint64_t tick) {
    impl_->game_tick = tick;
    impl_->instruction_budget = kTickInstructionBudget;
    impl_->budget_exhausted_this_tick = false;
}

void ScriptHost::dispatch(const SimEvent& event) {
    if (!scripting_active()) {
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
    sol::state_view lua(*impl_->lua);
    sol::table ev = lua.create_table();
    ev.set("name", std::string(name));
    fill_event_payload(*impl_, event, ev);
    // Index-bounded: a handler may register another handler for the same
    // event (push_back invalidates references); §2.3 says the new one
    // takes effect from the NEXT dispatched event.
    const size_t n = it->second.size();
    for (size_t hi = 0; hi < n && hi < it->second.size(); ++hi) {
        if (it->second[hi].disabled) {
            continue;
        }
        if (!run_handler(*impl_, it->second[hi], ev, name)) {
            it->second[hi].disabled = true;
        }
    }
}

void ScriptHost::end_tick(uint64_t tick) {
    if (!scripting_active()) {
        return;
    }
    // Phase 4: fire due timers ascending id (§3.6), then one GC step.
    if (!impl_->timers_frozen) {
        std::sort(impl_->timers.begin(),
                  impl_->timers.end(),
                  [](const TimerEntry& a, const TimerEntry& b) { return a.id < b.id; });
        // Index-based: a timer callback may call tb.timer (push_back),
        // reallocating the vector — never hold references across the call.
        // Callbacks appended this tick are NOT due (deadline is future).
        const size_t due_n = impl_->timers.size();
        for (size_t ti = 0; ti < due_n && ti < impl_->timers.size(); ++ti) {
            if (impl_->timers[ti].canceled || impl_->timers[ti].disabled ||
                impl_->timers[ti].deadline_tick != tick) {
                continue;
            }
            const uint64_t id = impl_->timers[ti].id;
            sol::protected_function_result r = impl_->timers[ti].fn(id);
            TimerEntry& t = impl_->timers[ti]; // re-acquire after the call
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
        impl_->timers.erase(std::remove_if(impl_->timers.begin(),
                                           impl_->timers.end(),
                                           [](const TimerEntry& t) { return t.canceled; }),
                            impl_->timers.end());
    }

    if (impl_->backglass_model.message_ticks_left > 0) {
        --impl_->backglass_model.message_ticks_left;
    }

    lua_gc(impl_->L, LUA_GCSTEP, 0); // §1.1 incremental step
}

void ScriptHost::fire_event(const char* name,
                            const EventInts& ints,
                            const EventStrings& strings,
                            const EventIntArrays& arrays) {
    if (!scripting_active() || name == nullptr) {
        return;
    }
    const auto it = impl_->handlers.find(name);
    if (it == impl_->handlers.end() || it->second.empty()) {
        return;
    }
    sol::state_view lua(*impl_->lua);
    sol::table ev = lua.create_table();
    ev.set("name", std::string(name));
    for (const auto& [k, v] : ints) {
        ev.set(k, v);
    }
    for (const auto& [k, v] : strings) {
        ev.set(k, v);
    }
    for (const auto& [k, v] : arrays) {
        sol::table arr = lua.create_table();
        for (int64_t x : v) {
            arr.add(x);
        }
        ev.set(k, arr);
    }
    const size_t n = it->second.size();
    for (size_t hi = 0; hi < n && hi < it->second.size(); ++hi) {
        if (it->second[hi].disabled) {
            continue;
        }
        if (!run_handler(*impl_, it->second[hi], ev, name)) {
            it->second[hi].disabled = true;
        }
    }
}

void ScriptHost::begin_game(int player_count) {
    impl_->player_count = std::clamp(player_count, 1, 4);
    impl_->current_player = 1;
    for (auto& ps : impl_->scores) {
        ps = PlayerScoreState{};
    }
    set_current_player(1);
    fire_event("game_start", {{"player_count", impl_->player_count}});
}

void ScriptHost::end_game() {
    // §4.3 game_end payload: scores by player + winning player index.
    int winner = 1;
    uint64_t best = 0;
    std::vector<int64_t> scores;
    for (int p = 1; p <= impl_->player_count; ++p) {
        const uint64_t s = player_scores(p).score;
        scores.push_back(int64_t(s));
        if (s > best) {
            best = s;
            winner = p;
        }
    }
    EventIntArrays arrays;
    arrays.emplace_back("scores", std::move(scores));
    fire_event("game_end", {{"winner", winner}}, {}, arrays);
}

} // namespace tb::sim
