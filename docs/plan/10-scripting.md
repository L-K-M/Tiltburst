# 10 — Scripting

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 05-engine-core.md (tick loop, PCG32 RNG, logging), 08-physics.md
(element behavior), 09-table-format.md (element ids, light groups, test-lab),
11-game-framework.md (game phases, framework↔script contract), 12-audio.md
(patch/song ids), 13-art-direction.md (light patterns, backglass, styles).

This document owns the exact signatures, payloads, and semantics of the `tb.*`
Lua API listed in PLAN.md §5.7. Sibling documents reference these names only;
if one disagrees with a signature or payload here, this document wins (below
canon).

## 1. Embedding

### 1.1 Runtime

- Lua **5.4** (vcpkg `lua`), bound via **sol2** (vcpkg `sol2`).
- Exactly one `lua_State` per running game, created at game start, destroyed
  at game end (or hot reload). Owned by the **sim thread**; no other thread
  may touch it, ever.
- Lua heap capped at **64 MiB** via a custom allocator passed to
  `lua_newstate`; allocation beyond the cap fails and surfaces as a Lua
  runtime error in the triggering handler (§2.5).
- GC is incremental: the host calls `lua_gc(L, LUA_GCSTEP, 0)` once per sim
  tick after script work. Never run a full collection on the sim thread.

### 1.2 Library whitelist

Open exactly these libraries, then strip as listed:

| Library | Opened | Removals / changes |
|---|---|---|
| base | yes | remove `load`, `loadstring`, `loadfile`, `dofile`; `print` redirected to engine log (info, `[lua]` prefix, rate-limited 100 lines/s); `collectgarbage` wrapped: only `"count"` permitted, anything else raises |
| math | yes | `math.random` / `math.randomseed` replaced by functions raising `"math.random is disabled; use tb.rng() / tb.rng_range(a,b)"` |
| string | yes | remove `string.dump` |
| table | yes | unchanged |
| coroutine | yes | see below |
| io, os, package (`require`), debug, utf8 | **no** | never opened; globals are `nil` |

**Decision — coroutine is allowed.** Coroutines are pure control flow: no
wall clock, no I/O, no nondeterministic state, so they cannot break
determinism, and they are the cleanest way to write multi-step mode
sequences. The only hazard: Lua 5.4 does **not** inherit debug hooks into
new coroutine threads, so the host wraps `coroutine.create` and
`coroutine.wrap` to install the watchdog hook (§2.4) on every new thread.

### 1.3 Setup sketch

```cpp
// src/table/script_host.cpp (sketch; sim-thread only)
sol::state lua(panic_handler, capped_alloc /* 64 MiB lua_Alloc */);
lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                   sol::lib::table, sol::lib::coroutine);
for (auto g : {"load", "loadstring", "loadfile", "dofile"}) lua[g] = sol::nil;
lua["string"]["dump"] = sol::nil;
lua["math"]["random"]     = [] { throw sol::error("math.random is disabled; use tb.rng()"); };
lua["math"]["randomseed"] = [] { throw sol::error("math.randomseed is disabled; use tb.rng()"); };
lua["print"]          = &ScriptHost::lua_print;          // -> tb_core log
lua["collectgarbage"] = &ScriptHost::lua_collectgarbage; // "count" only
wrap_coroutine_create(lua);   // installs watchdog hook on new threads (§2.4)
sol::table tb = lua.create_named_table("tb");
bind_tb_api(tb);              // every function in §3
lua.script_file(pack_dir / "rules.lua");   // top level, protected
```

`panic_handler` must never be reached (all entry points are protected
calls); if it fires anyway, log and disable all scripting for the rest of
the game rather than aborting the process.

### 1.4 Determinism of the Lua VM

- Build Lua with a **fixed hash seed**: define `luai_makeseed(L)` to return
  the constant `0x74696C74u`. Stock Lua 5.4 seeds string hashing from the
  clock and heap addresses, making `pairs()` order vary between runs — which
  silently breaks replay determinism.
- Style rule even so: iterate with `ipairs` or an explicit ordered key list
  whenever iteration has gameplay-visible effects.
- Script math is C doubles / 64-bit integers, deterministic per platform;
  canon §5.3 makes cross-platform bit-exactness a non-goal.

## 2. Execution model

### 2.1 Load and init

1. When the framework starts a new game (11-game-framework.md), the host
   creates the Lua state, binds `tb`, and runs `tables/<slug>/rules.lua`
   top-level code under a protected call (typically: define `CONFIG`,
   helpers, call `tb.on(...)`).
2. Compile or top-level error ⇒ the table fails to load; error + traceback
   logged and shown on the playfield overlay; framework returns to menu.
   `tb_validate` catches this offline.
3. If a global function `on_init` exists it is called once, protected, after
   the top level and before `game_start`. Optional; use it for work needing
   `tb.table_info` or initial light states.

### 2.2 Tick pipeline

Scripts run **synchronously on the sim thread, at tick granularity**. Every
1000 Hz tick executes exactly these **four phases**, in this order (binding;
05-engine-core.md and 11-game-framework.md state the same order):

```
tick N:
  1. late-latch input; apply the physical actions latched during tick N−1;
     physics integration + sim event generation (08-physics.md)
  2. dispatch the sim events generated in phase 1, in emission order;
     per event, run handlers in registration order
  3. step the GameFsm (11-game-framework.md §3); framework-originated
     events are dispatched to Lua SYNCHRONOUSLY as the FSM emits them,
     in emission order, inside this same phase
  4. fire expired script timers (deadline == N), ascending timer_id order;
     lua_gc STEP; publish SimSnapshot
```

**Phase 2 — sim-originated events.** Every §4.1 element event
(`switch_hit`, `target_down`, `bank_complete`, `spinner_spin`, `rollover`,
`kicker_enter`, `ramp_made`, `ball_lock`, `captive_full_travel`) plus
`ball_launched` and `drain` from §4.2. They fall out of the physics step,
so the tick that produces them is the tick the contact happened on.

**Phase 3 — framework-originated events.** `game_start`, `ball_start`,
`ball_end`, `player_up`, `game_end`, `tilt_warning`, `tilt`,
`multiball_start`, `multiball_end`, `ball_save_expired`, `timer_tick`.
The FSM does **not** queue these for the next tick: each handler runs to
completion inside the FSM step that emitted the event, before the FSM
proceeds. That is exactly what makes 11-game-framework.md §4.5 workable —
scripts may call `tb.add_bonus` inside the `ball_end` handler and the
framework counts the updated total, because it reads the accumulator only
after the handler returns. Framework event emission order is part of the
deterministic replay record (16-testing-ci.md).

A last-ball drain therefore delivers its whole chain within one tick, in
phase order: `drain` (phase 2, sim) → `multiball_end` → `ball_end`
(phase 3, as the FSM consumes that drain and takes T10 into BonusCount —
11-game-framework.md §2.2/§2.3). A drain that leaves a ball pending or in
the plunger lane is accounting-only (T11) and produces no phase-3 event.

No concurrency, no reentrancy: a handler runs to completion before the next
starts, in phases 2, 3 and 4 alike. **Action latency:** bookkeeping actions
(`tb.score`, lights, `tb.state`, timers, messages, sounds, music,
backglass, extra-ball awards) take effect immediately and are visible to
Lua later in the same tick. Physical actions (`tb.kick`, `tb.kick_hold`,
`tb.release_lock`, `tb.magnet_*`, `tb.set_flipper_enabled`, `tb.add_ball`,
`tb.drop_bank_reset`, `tb.gate_*`) are latched and applied at the start of
tick N+1's phase 1, keeping physics state immutable while scripts run —
this holds identically for calls made from phase-3 and phase-4 handlers.
Events caused by a physical action (e.g. `ball_launched` after
`tb.add_ball`) fire on the later tick where they physically occur.

### 2.3 Registration: `tb.on`

```lua
tb.on(event_name, handler)   -- returns nothing
```

`event_name`: string, one of the canon event names (§4); unknown → error.
`handler`: called as `handler(ev)`, `ev` a table with `ev.name` plus the §4
payload fields. Multiple handlers per event run in registration order. There
is no `tb.off`; make a handler inert via a flag in `tb.state`. Registering
inside another handler is legal and takes effect from the next dispatched
event.

### 2.4 Instruction watchdog

- Shared budget of **10,000 Lua VM instructions per tick**, reset at the
  start of phase 2 and drawn down by every handler and timer callback in
  phases 2, 3 and 4 of that tick.
- The budget is a **time** budget expressed in instructions, and the two
  numbers are tied: at the ~100 M instr/s ceiling of the sandboxed 5.4 VM
  on reference hardware, 10,000 instructions ≈ **100 µs** — exactly
  ADR-006's revisit criterion ("scripting still completes in < 100 µs") and
  a tenth of the 1 ms tick. Changing the instruction count means restating
  the microsecond equivalence here and in ADR-006; a budget whose time
  equivalent does not fit inside the tick is a bug, not a tuning choice.
  A full event burst in the shipped tables measures under 3,000
  instructions (~30 µs), so 10,000 leaves ~3× headroom over real content
  while still tripping long before a runaway script can push the tick into
  the 05-engine-core.md §6.1 overrun clamp. A 40,000-iteration loop is
  aborted in its first tick instead of quietly costing ~400 µs every tick
  forever.
- Enforced with `lua_sethook(L, hook, LUA_MASKCOUNT, 1000)`: the hook fires
  every 1,000 instructions (unchanged — the budget is 10 hook fires, so the
  worst-case overshoot past the cap is one interval, ~10 µs), decrements
  the tick budget, and at zero raises the Lua error
  `"instruction budget exceeded"`. The hook must be installed on the main
  state **and every coroutine** (§1.2).
- On overrun: the running handler/callback is aborted by that error,
  **permanently disabled for the rest of the game** (a disabled timer
  callback also cancels its timer), the error is logged with traceback,
  remaining handler invocations for that tick are skipped, and the game
  continues. Budget refills next tick. Disabling is deterministic: same
  inputs → same overrun at the same tick.

### 2.5 Runtime errors

Every entry into Lua (top level, `on_init`, handlers, timer callbacks) is a
`sol::protected_function` call. On error:

1. The failing invocation is aborted; bookkeeping actions it already
   performed stand (no rollback).
2. Error + traceback logged (error level); identical (handler, message)
   pairs logged at most once per 5 s.
3. The handler is **not** disabled for ordinary runtime errors — unlike
   budget overruns — except: 10 consecutive failing invocations disable it
   with a distinct log line.
4. Remaining handlers for the same event still run. The game never crashes,
   pauses, or skips a physics tick because of a script error.
5. sol2 must be configured so no C++ exception ever escapes the protected
   boundary into the sim loop.

### 2.6 Determinism obligations (restated)

Canon §5.3: same binary + same seed + same input stream ⇒ identical
simulation. For scripts: they run only on the sim thread at tick boundaries
(§2.2); the API exposes no render, wall-clock, audio, or thread-timing
observables; all randomness goes through `tb.rng` / `tb.rng_range`
(the `rng_script` PCG32 stream, 05-engine-core.md §10.1), whose draw order
within that stream is part of the deterministic simulation — do not draw
"just in case". The determinism suite
(16-testing-ci.md) replays recorded input and asserts identical event +
script-action logs.

### 2.7 Hot reload (`--dev` only)

With `--dev`, the main thread polls the loaded table's `rules.lua` mtime
every 500 ms. On change: the sim finishes its current tick, the current game
is **aborted** (no scores saved), the Lua state is destroyed, and a fresh
game starts with the same player count and a fresh RNG seed. **Script state
is not preserved** — the game restarts cleanly from `game_start`; do not
attempt to migrate `tb.state`. If the changed file fails to compile or its
top level errors: log + show the error on the playfield overlay, keep the
previous rules running, keep watching.

## 3. API reference

All `tb.*` functions raise a Lua error for wrong argument **types** (caught
per §2.5). Out-of-range **values** and unknown element ids clamp or
warn-and-no-op as specified per function ("warn" = one rate-limited log
line). Ids are the string `id` fields from `table.json` (09-table-format.md).

### 3.1 Scoring

#### `tb.score(points)`

Adds `points` to the current player's score, immediately. `points`: integer
≥ 0; non-integers floored; negative → warn, no-op. No return. During tilt
(from `tilt` until the next `ball_start`) calls are silently ignored.
`tb.set_multiplier` does **not** affect `tb.score`; playfield multipliers
are a script concern (multiply the argument yourself, §5.5).

```lua
tb.on("switch_hit", function(ev)
  if ev.id == "pop_main" then tb.score(500) end
end)
```

#### `tb.add_bonus(points)`

Adds `points` (same validation as `tb.score`) to the current player's
end-of-ball bonus accumulator. The framework awards
`bonus × bonus_multiplier` in the bonus-count sequence after `ball_end`
(11-game-framework.md), then zeroes the accumulator.

```lua
tb.add_bonus(1000)   -- worth 1000 × multiplier at end of ball
```

#### `tb.set_multiplier(n)`

Sets the current player's **bonus multiplier**. `n`: integer, clamped to
1–10; resets to 1 at each `ball_start`.

```lua
tb.set_multiplier(2); tb.show_message("BONUS 2X", { style = "mode" })
```

### 3.2 Lights

Light ids and light groups come from `table.json`. Light commands are
idempotent and take effect immediately. Unknown id → warn, no-op.

#### `tb.light_on(id)` / `tb.light_off(id)`

Sets a light (or every light in a group, if `id` names a group) steady
on / off, canceling any blink pattern on it.

```lua
tb.light_on("light_scoop"); tb.light_off("grp_top_lanes")
```

#### `tb.light_blink(id, pattern)`

Starts a repeating pattern on light or group `id`; runs until `light_on` /
`light_off` on the same id. Pattern phase is anchored to the tick the call
takes effect (replay-identical). Canonical patterns (visual treatment in
13-art-direction.md; timing normative here):

| `pattern` | Timing (exact) |
|---|---|
| `"slow_blink"` | 500 ms period, 250 ms on / 250 ms off |
| `"fast_blink"` | 200 ms period, 100 ms on / 100 ms off |
| `"strobe"` | 100 ms period, 20 ms on / 80 ms off |
| `"chase"` | group only: one lamp lit at a time, advancing every 80 ms in group declaration order |
| `"breathe"` | 1600 ms period, sinusoidal brightness 0→1→0 |

`"chase"` on a non-group id → warn, falls back to `"fast_blink"`. Unknown
pattern name → error.

```lua
tb.light_blink("light_jackpot", "strobe")
tb.light_blink("grp_top_lanes", "chase")
```

### 3.3 Sound and music

Patch/song ids come from `audio.json` (12-audio.md). Unknown id → warn,
no-op. Sounds are scheduled sample-accurately from the sim tick (canon §5.4).

#### `tb.play_sound(patch_id, opts)`

Fire-and-forget one-shot; no return; polyphony per 12-audio.md. `opts`
(table, optional):

| Field | Type | Default | Meaning |
|---|---|---|---|
| `duck` | boolean | `false` | Duck the music bus under this sound. Sets bit0 of the emitted `SoundEvent` flags (12-audio.md §4.1), which ramps the music bus **−6 dB (×0.501) over 50 ms**, holds until **200 ms** after the most recent trigger, then ramps back to unity over **50 ms** (12-audio.md §10). |

The built-in patches `jackpot_hit`, `multiball_riser`, `extra_ball_fanfare`,
`tilt_alarm`, and `drain_womp` duck on their own; `{duck = true}` is the
only way a table extends that trigger list (12-audio.md §10).

```lua
tb.play_sound("sfx_skill_shot")
tb.play_sound("sfx_mode_start", { duck = true })   -- music dips under it
```

#### `tb.play_music(song_id)` / `tb.stop_music()`

Starts tracker song `song_id` (100 ms equal-power crossfade if another song
is playing, the new song always from `order[0]` row 0; no-op if the same
song already plays). `tb.stop_music()` fades out over the same 100 ms.

Song ids are the reserved music-state vocabulary of 12-audio.md §9 —
`attract`, `main`, `mode`, `multiball`, `wizard`, `game_over` — plus any
additional ids the table's `audio.json` defines (e.g. a table-flavored mode
song). A table may define any subset; a missing id means silence in that
state. The framework auto-plays `attract` and `game_over`; scripts select
the rest.

```lua
tb.on("multiball_start", function() tb.play_music("multiball") end)
tb.on("multiball_end",   function() tb.play_music("main") end)
```

### 3.4 Physical elements

All functions here latch to the next tick (§2.2).

#### `tb.kick(kicker_id, speed, angle_deg)`

Ejects the ball held by kicker `kicker_id` (must currently hold a ball, else
warn, no-op), canceling the pending auto-eject (below). `speed`: number,
optional, default the element's `eject_speed`; clamped to 0–12 m/s (canon
max ball speed). `angle_deg`: number, optional, default the element's
`eject_angle_deg`; direction in playfield coordinates, degrees CCW from +x
(canon §5.3); 90 = straight up-table.

**Kicker lifecycle (normative; physics in 08-physics.md §6.9).** On capture
the sim fires `kicker_enter` and starts the element's `capture_ms` countdown
(09-table-format.md §4.12; default 800 ms). If the countdown expires, the
ball auto-ejects with the element's default `eject_speed` /
`eject_angle_deg` — a rules file that does nothing can never softlock a
ball. Scripts eject earlier (or with different parameters) via `tb.kick`,
or take over entirely via `tb.kick_hold`.

#### `tb.kick_hold(kicker_id)`

Cancels the pending `capture_ms` auto-eject on kicker `kicker_id` (must
currently hold a ball, else warn, no-op): the ball becomes **script-held**,
indefinitely, until `tb.kick` (08-physics.md §6.9, `hold_ticks = 0`).
Holding deliberately opts out of the auto-eject failsafe, so every
`tb.kick_hold` needs a guaranteed later `tb.kick` path — a timer, an event,
or a button (e.g. Cosmic Carnival's cannon holds until either flipper
button `switch_hit` or a 5 s `tb.timer`, 15-launch-tables.md §4.3).

**A hold never survives a tilt.** On `tilt` — and identically on Duel
timeout — the framework immediately commands every CAPTURED ball to eject:
kickers (including script-held ones) and ball locks, at the element's
default `eject_speed` / `eject_angle_deg`; locked balls release too
(11-game-framework.md §5). "Kickers and magnets de-energized" on tilt means
no *new* captures and no scripted kicks; the sim's `capture_ms` auto-eject
keeps running during tilt and is never suppressed. This matters because
script timers are frozen during tilt (§3.6) and button `switch_hit`s are
suppressed (§4.1) — the two release paths a hold usually relies on. During
normal play, a hold whose release path never fires is recovered by the
framework's zero-free-ball watchdog after 30,000 ticks (pitfall 11,
11-game-framework.md §4.6): recovered and logged as an error, never a
hang — but still an author bug.

```lua
tb.on("kicker_enter", function(ev)
  if ev.id == "scoop" then
    tb.kick_hold("scoop")   -- take over from the capture_ms auto-eject
    tb.timer(1200, function() tb.kick("scoop", 4.0, 75) end)
  end
end)
```

#### `tb.release_lock(lock_id, count)` → `released`

Releases up to `count` (integer ≥ 1) balls from ball-lock element `lock_id`
into play, one per 500 ms. Returns the integer actually released (0 if
empty or unknown id, with a warn). Eject kinematics (`eject_speed`,
`eject_angle_deg`, push-out) are 08-physics.md §6.14's; the 500 ms cadence
is the same number there and in 09-table-format.md §4.16.

**Lock ownership (normative; physics in 08-physics.md §6.14).** The sim
captures **unconditionally**: a FREE ball entering an unfilled lock mouth is
taken, `switch_hit` then `ball_lock{lock_id, count}` fire, and the ball is
out of play. There is **no confirm API** — a script cannot refuse, veto, or
pre-authorize a capture, and nothing in `tb.*` does so.

**Mandatory unlit-lock pattern.** Because the sim never asks, every table
with a lock must handle the case where the lock is *not lit*: in the
`ball_lock` handler, immediately call `tb.release_lock(id, 1)` to hand the
ball straight back (§5.5). This is the only correct way to say "not now".

**Sim failsafe (mirrors the kicker `capture_ms` failsafe above).** If a locked ball is
neither released nor claimed within **3000 ms** of its capture, the sim
auto-releases **one** ball and logs a warning. A capture counts as *claimed*
the moment a registered `ball_lock` handler for it runs to completion
without releasing it — that is the script taking responsibility for the ball
(this document owns the definition of *claimed*; 08-physics.md §6.14 owns
the timer). A table that registers no `ball_lock` handler, or whose handler
raised (§2.5) or is disabled (§2.4), therefore gets its ball back after 3 s
instead of losing it — the same "a rules file that does nothing can never
softlock a ball" guarantee kickers have. Deliberate long holds (locking ball
1 of 3 for minutes) are claimed on capture and are never touched by the
failsafe.

```lua
local n = tb.release_lock("lock_main", 3)   -- may be < 3
```

#### `tb.magnet_on(id)` / `tb.magnet_off(id)` / `tb.magnet_pulse(id, ms)`

Energizes / de-energizes magnet `id` (field model in 08-physics.md).
`magnet_pulse` energizes for exactly `ms` milliseconds (integer, clamped
1–10,000) then releases; a second pulse before expiry restarts the window.
`magnet_on` without `magnet_off` is legal, but the framework force-releases
all magnets at `ball_end` and on `tilt`.

```lua
tb.magnet_pulse("magnet_drift", 400)   -- grab-and-fling
```

#### `tb.set_flipper_enabled(id, enabled)`

`enabled`: boolean. Disabled flippers ignore input and return to rest. The
framework force-disables on `tilt` and re-enables at `ball_start`; script
overrides must be re-applied each `ball_start` by the script itself.

```lua
tb.set_flipper_enabled("flipper_upper", false)
```

#### `tb.drop_bank_reset(id)`

Raises all targets of drop bank `id`. If a ball overlaps a raising target
the reset defers until clear (08-physics.md). Banks with
`"reset": "auto"` in table.json reset themselves `auto_reset_ms` after
`bank_complete` (09-table-format.md §4.8); calling this on them is harmless.

```lua
tb.on("bank_complete", function(ev)
  tb.timer(1000, function() tb.drop_bank_reset(ev.bank_id) end)
end)
```

#### `tb.gate_open(id)` / `tb.gate_close(id)`

Opens/closes a controlled gate. Purely mechanical one-way gates
(09-table-format.md) ignore these calls with a warn.

```lua
tb.gate_open("gate_orbit")   -- let orbit shots through
```

### 3.5 Ball management

#### `tb.ball_save(ms, uses)`

Arms (or re-arms) ball save (framework mechanism, 11-game-framework.md
§4.3): while armed, a drain consumes one use and auto-relaunches with no
`ball_end`. `ms`: integer; **`0` disarms immediately** (`uses` ignored) —
the only way to shorten a window; otherwise clamped 1–120,000 and repeat
calls set remaining time to `max(remaining, ms)` and remaining uses to
`max(remaining_uses, uses)` — never less. `uses`: optional integer, default
1, clamped 1–10 (multi-use saves are the multiball pattern, §5.5).
Script-armed saves count from the tick the call takes effect; the
framework's default single-use save (8 s from `ball_launched`, 11 §4.3)
arms through the same mechanism and is extended or disabled identically.
The framework lights the shoot-again light while armed and fires
`ball_save_expired` only when the time window lapses; consuming the last
use disarms silently (§4.2). Grace periods are a script pattern (§5.6),
not built in.

```lua
tb.on("ball_start", function() tb.ball_save(8000) end)        -- single-use
tb.on("multiball_start", function() tb.ball_save(10000, 3) end)
```

#### `tb.add_ball(n)`

Requests `n` balls (optional integer ≥ 1, default 1; non-integers floored)
auto-launched from the trough into play, served one at a time (plunger
auto-fire, no player action; 11-game-framework.md §4.4). Bounded by trough
contents: shortfall → warn, serves what it can; trough empty (all 4
physical balls in play or held) → warn, no-op. Fires no events itself; the
framework fires `multiball_start`/`multiball_end` on balls-in-play
transitions (§4.2).

```lua
tb.add_ball(2)   -- e.g. bring a 3-ball multiball up from one ball
```

#### `tb.award_extra_ball()`

No arguments, no return; takes effect immediately (bookkeeping, §2.2).
Extra balls are a framework mechanism (11-game-framework.md §3.3): the
current player's stacked extra balls increase by one, capped at 3; awards
past the cap post 100,000 base points at ×1 multiplier instead, and in
Duel every award converts to points. The framework shows "EXTRA BALL" on
the backglass and plays the built-in `extra_ball_fanfare` patch
(12-audio.md §7.1); at end of ball a stacked extra ball makes the same
player replay the same ball number ("SHOOT AGAIN"). Scripts decide only
*when* to award.

```lua
tb.on("bank_complete", function(ev)
  if ev.bank_id == "bank_boost" then tb.award_extra_ball() end
end)
```

### 3.6 Timers

Ticks are milliseconds: the sim runs at exactly 1000 Hz (canon §5.3), so
`ms` values are exact tick counts with no rounding.

#### `tb.timer(ms, fn, repeats)` → `timer_id`

| Param | Type | Notes |
|---|---|---|
| `ms` | integer | delay per firing in ticks; clamped 1–3,600,000 |
| `fn` | function | called `fn(timer_id)` in phase 4 of the tick pipeline (§2.2) |
| `repeats` | integer, optional | total firings; default 1; 0 = infinite; self-cancels after the last |

Returns integer `timer_id`, unique per game, strictly increasing in creation
order (this order breaks same-tick ties, §2.2).

**Pause semantics (normative):** timers advance only while the game phase is
`BALL_IN_PLAY` and not tilted (phases in 11-game-framework.md). They are
frozen during the bonus count, between balls, during tilt, and while paused —
a 5000 ms mode timer means 5000 ms of *live play*.

Freezing timers during tilt cannot strand a ball: on `tilt` the framework
force-ejects every CAPTURED ball — kickers, including balls held via
`tb.kick_hold`, and ball locks — at the element's default `eject_speed` /
`eject_angle_deg`, and locked balls release too (§3.4,
11-game-framework.md §5). So the release timer a hold was waiting on may
never fire, yet the ball is already back in play. The same force-eject runs
on Duel timeout.

All timers are canceled automatically after the `ball_end` handlers of the
ball in which they were created have run (that cancellation happens inside
the same phase-3 FSM step, §2.2); modes spanning balls store progress in
`tb.state` and re-arm timers in `ball_start`.

#### `tb.cancel_timer(timer_id)`

Cancels a pending timer. Canceling a fired, canceled, or unknown id is a
silent no-op (cancel-after-fire races are harmless).

```lua
local t = tb.timer(5000, function() tb.light_off("light_skill") end)
tb.on("rollover", function(ev)
  if ev.id == "lane_a" then tb.cancel_timer(t) end
end)
```

### 3.7 Display

#### `tb.show_message(text, opts)`

Shows `text` (string, ≤ 64 chars, longer truncated with a warn) on the
playfield message overlay and backglass message area. A new message replaces
the current one immediately. `opts` (table, optional):

| Field | Type | Default | Values |
|---|---|---|---|
| `style` | string | `"info"` | `"info"`, `"mode"`, `"jackpot"`, `"warning"` (visuals in 13-art-direction.md) |
| `duration_ms` | integer | 2000 | clamped 250–10,000 |

```lua
tb.show_message("HURRY UP!", { style = "warning", duration_ms = 3000 })
```

#### `tb.backglass` sub-API

The backglass renders autonomously from snapshots (canon §5.4); these calls
set declarative state it reads, effective immediately in snapshot terms.

| Function | Behavior |
|---|---|
| `tb.backglass.set_layout(layout_id)` | Switch layout. Canonical ids: `"scores"` (default in play), `"mode"` (big mode title + countdown, scores small), `"celebration"` (full-screen animation stage). Unknown → warn, no-op. The `"attract"` layout is framework-owned, outside games. |
| `tb.backglass.focus_score(player)` | Highlight player `player` (integer 1–4)'s score. Called automatically by the framework on `player_up`; scripts may override. |
| `tb.backglass.animate(name)` | One-shot animation. Canonical trigger names: `"jackpot"`, `"super_jackpot"`, `"multiball_intro"`, `"mode_start"`, `"mode_complete"`, `"extra_ball"`, `"ball_save"`, `"high_score"`, `"tilt"`. Unknown → warn, no-op. Per-table definitions in art.json (13-art-direction.md); missing ones fall back to a built-in generic per trigger. |

```lua
tb.backglass.set_layout("celebration"); tb.backglass.animate("super_jackpot")
```

### 3.8 State and game info

Canon §5.7 lists `tb.state`, `tb.game` and `tb.table_info` in the same
sentence as the callable actions, and those three spellings are exactly the
spellings bound here. All three are **tables, not functions**: `tb.state` is
read-write and per-player, `tb.game` (read-only session info) and
`tb.table_info` (read-only table metadata) raise on any write. Counting
`tb.backglass` (§3.7 — a table of functions whose sub-names are owned by
that section, not by canon), **exactly four canon §5.7 names are tables**;
every other name on the §5.7 action list, `tb.rng` and `tb.rng_range` (§3.9)
included, is a plain function. So `Api.EveryCanonNameExists`
(04-milestones.md M9), which enumerates the §5.7 lists and fails on a
missing *or* extra name, must accept a `table` for those four: asserting
`type(tb.<name>) == "function"` across the whole action list would fail a
correct implementation, and exposing `tb.game()` / `tb.table_info()` as
getters to satisfy such a check contradicts this section.

#### `tb.state`

Read-write Lua table, **per-player**: the host keeps one backing table per
player and swaps which one `tb.state` proxies on every `player_up`. Contents
persist across that player's balls, are fresh empty tables at `game_start`,
and are dropped at game end. Store only booleans, numbers, strings, and
plain tables. During `on_init` and before the first `player_up`, `tb.state`
proxies player 1.

```lua
tb.on("ball_start", function()
  tb.state.combo = tb.state.combo or 0   -- survives ball-to-ball
end)
```

#### `tb.game`

Read-only table (writes raise an error):

| Field | Type | Meaning |
|---|---|---|
| `current_player` | integer | 1-based |
| `player_count` | integer | 1–4 |
| `ball_number` | integer | 1-based, current player's ball |
| `balls_per_game` | integer | from settings, default 3 |
| `score` | integer | current player's score |
| `tick` | integer | sim ticks (= ms) since `game_start` |

Tiltburst is **free-play**: there is no credits field and scripts must not
simulate coin mechanics.

```lua
if tb.game.ball_number == tb.game.balls_per_game then
  tb.show_message("LAST BALL", { style = "warning" })
end
```

#### `tb.table_info`

Read-only table (writes raise an error, exactly as for `tb.game`): `name`
(string), `slug` (string), `width_m` (number), `height_m` (number) from
`table.json` metadata (09-table-format.md). Populated before `on_init`
(§2.1) and constant for the life of the game.

```lua
print("loaded " .. tb.table_info.name)
```

### 3.9 Randomness

Both draw from `rng_script`, the sim-owned PCG32 stream dedicated to
scripts (05-engine-core.md §10.1): seeded deterministically at game start
from the master seed (`seed(game_seed, 0x0000000000000002)`), and separate
from the `rng_sim` stream physics uses, so script draws never perturb
physics sequences (08-physics.md §2.2). Still fully deterministic under
replay (§2.6) — draw order within the script stream is part of the
deterministic simulation. Canon §5.7 lists this surface as `tb.rng`;
`tb.rng_range` is part of it.

| Function | Returns | Errors |
|---|---|---|
| `tb.rng()` | number in [0, 1) | — |
| `tb.rng_range(a, b)` | uniform integer in [a, b] inclusive | error if not integers or `a > b` |

```lua
local award = awards[tb.rng_range(1, #awards)]
```

## 4. Event catalog

Handler argument: one table `ev` with `ev.name` plus the fields below.
`ball_id` is the sim's stable integer id for a physical ball (1–4). `tags`
is an array of strings copied from the element's `"tags"` in table.json
(may be empty, never nil).

**Firing rule:** every physical actuation of a scriptable element fires
`switch_hit` first, then its specialized event (if any) immediately after,
same tick. Subscribe to the specific event normally; subscribe to
`switch_hit` once for frenzy-style "any switch" logic. The single exception
is `captive_full_travel`, which reports a *later* physical outcome of the
same strike and therefore arrives on a later tick (§4.1).

### 4.1 Element events

| Event | Payload | Fires when |
|---|---|---|
| `switch_hit` | `{id, ball_id, speed, tags}` | any scoring contact: slingshot fire, pop bumper fire, standup hit, rollover pass, spinner revolution, drop target hit, kicker capture, ball-lock capture, ramp exit sensor, **captive-ball strike** (below), and **cabinet buttons** (below). `speed` = ball speed in m/s at contact — for a captive-ball strike, the impact speed of the *striking* free ball; 0 for buttons. |
| `target_down` | `{bank_id, target_index}` | a drop target falls; `target_index` 1-based in the bank's declaration order |
| `bank_complete` | `{bank_id}` | last standing target of a drop bank falls (after its `target_down`); auto-reset banks reset after this event is dispatched |
| `spinner_spin` | `{id, rpm}` | once per full revolution; `rpm` = instantaneous revolutions/minute |
| `rollover` | `{id, ball_id}` | ball passes over a rollover (lanes, in/outlanes) |
| `kicker_enter` | `{id, ball_id}` | kicker captures a ball (auto-ejects `capture_ms` later unless the script kicks or holds, §3.4) |
| `ramp_made` | `{id, ball_id}` | ball crosses a ramp's exit sensor (rollbacks never fire this) |
| `ball_lock` | `{lock_id, count}` | ball-lock element captures a ball (unconditionally — §3.4); `count` = balls now held there |
| `captive_full_travel` | `{id}` | the captive ball of `captive_ball` element `id` reaches the far end `b` of its slot at ≥ 0.3 m/s (08-physics.md §6.13). Fires on a later tick than the strike's `switch_hit` — see below |

**Captive ball (canon §5.7).** A `captive_ball` produces two distinct,
independent events, and no payload flag ties them together:

- The strike fires the standard `switch_hit{id, ball_id, speed, tags}`,
  with `ball_id` = the striking free ball and `speed` = **that ball's
  impact speed**. This is the number tables threshold on (Atomic Diner's
  SHAKE counts captive hits at ≥ 0.8 m/s, 15-launch-tables.md §2.5).
- `captive_full_travel{id}` fires when the captive ball itself reaches the
  far end `b` of its slot (09-table-format.md §4.15) at ≥ 0.3 m/s. Payload
  is the element id only: the captive is not a `ball_id`-addressable free
  ball, and the striking ball may already be elsewhere.

Timing, worked for Atomic Diner's `shaker` (slot `a` [0.085, 0.560] → `b`
[0.085, 0.640], i.e. 0.080 m straight up-table): the captive's center
travels `|b − a| − 2r` = 0.080 − 0.027 = **0.053 m** against a deceleration
of `g·sin(6.5°) + μ_rr·g·cos(6.5°)` = 1.1105 + 0.2437 = **1.3542 m/s²**
(08-physics.md §1.3), and an in-line strike hands the captive **0.95 ×** the
striking ball's impact speed (08-physics.md §6.13: equal masses, e = 0.9).
So on that element the minimum impact speed that fires
`captive_full_travel` is `√(0.3² + 2·1.3542·0.053) / 0.95` = **0.51 m/s**; a
0.8 m/s SHAKE-threshold hit reaches `b` at 0.66 m/s **75 ticks** later; a
hard 2.0 m/s hit reaches it at 1.86 m/s **28 ticks** later. Across the
09 §4.15 slot range (0.040–0.120 m) the travel distance is 0.013–0.093 m,
so the gap is a few ticks to a few hundred. Never assume the two events
share a tick, and never assume no other events land between them; a soft
hit fires `switch_hit` with no `captive_full_travel` at all.

**Cabinet buttons as switches:** each button press (not release, not
autorepeat) fires `switch_hit` with `ball_id = 0`, `speed = 0`,
`tags = {"button"}`. Ids: `"button_flipper_left"` / `"button_flipper_right"`
for the flipper buttons — this is how lane change (§5.7) works — and
`"button_launch"` for the launch/plunger action (05-engine-core.md input
action `plunger`). `button_launch` is the **launch/plunger action only**;
table mechanics that need a player *choice* read the flipper buttons
instead, because left/right is load-bearing there: Cosmic Carnival's cannon
aims and fires on `button_flipper_left`/`_right` (15-launch-tables.md §4.3)
and Voltage Vandals' hatch purchase reads the flipper button on the side
being bought (15-launch-tables.md §5.3). `button_launch` fires on
every press: a press with a ball in the plunger lane also charges the
physical plunger as normal, so scripts gate the event on their own state.
Presses are reported even while a flipper is disabled, but not during tilt.

### 4.2 Ball flow events

| Event | Payload | Fires when |
|---|---|---|
| `ball_launched` | `{ball_id}` | a ball leaves the plunger lane into the playfield (player plunge or auto-launch) |
| `drain` | `{ball_id, balls_remaining}` | a ball enters the outhole; `balls_remaining` = balls still in play after it. The framework then decides: ball-save relaunch, multiball continue, or end of ball. |
| `ball_save_expired` | `{}` | the ball-save time window lapses while still armed (time expiry only; consuming the last use disarms without firing this, §3.5) |
| `multiball_start` | `{ball_count}` | balls in play rises from 1 to 2 (framework-fired); `ball_count` = balls now in play |
| `multiball_end` | `{}` | balls in play returns to 1 (framework-fired) |

### 4.3 Game lifecycle events

| Event | Payload | Fires when |
|---|---|---|
| `game_start` | `{player_count}` | new game begins, after `on_init`, before the first `ball_start` |
| `ball_start` | `{player, ball_number}` | a ball is delivered to the plunger lane for normal play (not for ball-save relaunches or added balls) |
| `ball_end` | `{player, ball_number, bonus, bonus_multiplier}` | last ball in play drains with no save; fires **before** the framework's bonus count; the fields are what will be counted |
| `player_up` | `{player, previous_player}` | after `tb.state` swaps to `player`, before that player's `ball_start`; also for player 1 before ball 1 (`previous_player = 0`) |
| `game_end` | `{scores, winner}` | after the final `ball_end` + bonus count; `scores` = array of final scores by player; `winner` = lowest player index holding the max score |
| `tilt_warning` | `{count}` | nudge threshold exceeded; `count` = warnings so far this ball (threshold in 11-game-framework.md) |
| `tilt` | `{}` | tilt: the framework has already disabled flippers, zeroed this ball's bonus, and begun ignoring `tb.score` until next `ball_start` |
| `timer_tick` | `{ball_time_s}` | once per 1000 ticks of unpaused `BALL_IN_PLAY` time; `ball_time_s` = whole seconds of live play this ball. A shared 1 Hz heartbeat for countdown displays — unrelated to `tb.timer`, which is the precision mechanism and fires callbacks, not events. |

### 4.4 Framework vs. script responsibilities

The authoritative contract table lives in 11-game-framework.md. Summary:

- The **sim** emits the §4.1 element events plus `ball_launched` and
  `drain` (§4.2); scripts see them in phase 2 (§2.2). There is no
  trough-entry switch — the sim removes the ball at the outhole and emits
  `drain{ball_id, balls_remaining}` (08-physics.md §6.15), and the
  framework decrements its own count on that event.
- The **framework** consumes `drain` for ball accounting (ball save,
  multiball continue, end of ball) and consumes the sim's neutral
  `danger_threshold` (11-game-framework.md §5 — not a script event). It
  *emits* `multiball_start`, `multiball_end` and `ball_save_expired` from
  §4.2 plus every §4.3 lifecycle event, `tilt_warning` and `tilt` included;
  scripts see all of these in phase 3 (§2.2).
- **Scripts** never re-implement ball accounting: they react for scoring,
  modes, lights, and sound. All events are still delivered to scripts,
  including framework-consumed ones — a script seeing `drain` must not call
  `tb.add_ball` to "fix" it (that is ball save's job).

## 5. Patterns cookbook

Complete, self-contained snippets. Adapt element ids; keep structure.

### 5.1 Skill shot with timeout

```lua
-- Light one random top lane at launch; hit it within 5 s to score big.
local skill_timer = nil
tb.on("ball_launched", function()
  tb.state.skill_lane = ({"lane_a", "lane_b"})[tb.rng_range(1, 2)]
  tb.light_blink("light_" .. tb.state.skill_lane, "fast_blink")
  skill_timer = tb.timer(5000, function()          -- window closes
    tb.light_off("light_" .. tb.state.skill_lane)
    tb.state.skill_lane = nil
  end)
end)
tb.on("rollover", function(ev)
  if ev.id == tb.state.skill_lane then
    tb.cancel_timer(skill_timer)
    tb.light_off("light_" .. ev.id)
    tb.state.skill_lane = nil
    tb.score(25000)
    tb.show_message("SKILL SHOT 25,000", { style = "jackpot" })
  end
end)
```

### 5.2 Combo chain with decay timer

```lua
-- Consecutive ramps within 4 s each raise the combo value.
local combo_timer = nil
tb.on("ramp_made", function(ev)
  tb.state.combo = (tb.state.combo or 0) + 1
  tb.score(5000 * tb.state.combo)                  -- 5k, 10k, 15k, ...
  tb.show_message("COMBO x" .. tb.state.combo, { style = "mode" })
  tb.cancel_timer(combo_timer)                     -- restart decay window
  combo_timer = tb.timer(4000, function() tb.state.combo = 0 end)
end)
```

### 5.3 Hurry-up with decreasing value

```lua
-- Counts down from 50,000 by 1,000 every 100 ms; collect at the scoop.
tb.on("bank_complete", function(ev)
  if ev.bank_id ~= "bank_gears" then return end
  tb.state.hurry = 50000
  tb.light_blink("light_scoop", "strobe")
  tb.state.hurry_timer = tb.timer(100, function()  -- repeats forever
    tb.state.hurry = math.max(tb.state.hurry - 1000, 5000)  -- floor 5,000
    if tb.state.hurry == 5000 then tb.cancel_timer(tb.state.hurry_timer) end
  end, 0)
end)
tb.on("kicker_enter", function(ev)
  if ev.id == "scoop" and tb.state.hurry then   -- else: capture_ms auto-eject
    tb.kick_hold("scoop")
    tb.cancel_timer(tb.state.hurry_timer)
    tb.score(tb.state.hurry)
    tb.show_message("HURRY-UP " .. tb.state.hurry, { style = "jackpot" })
    tb.state.hurry = nil
    tb.light_off("light_scoop")
    tb.timer(1200, function() tb.kick("scoop", 4.0, 75) end)
  end
end)
```

### 5.4 Mode ladder with mini-wizard

```lua
-- Modes start in a fixed order at the scoop; finish all three for wizard.
local MODES = { "mode_speed", "mode_drift", "mode_boost" }  -- ordered
tb.on("kicker_enter", function(ev)
  if ev.id ~= "scoop" then return end
  tb.kick_hold("scoop")
  tb.state.done = tb.state.done or {}
  local next_mode = nil
  for _, m in ipairs(MODES) do                     -- ipairs: deterministic
    if not tb.state.done[m] then next_mode = m; break end
  end
  if next_mode then
    start_mode(next_mode)                          -- table-specific
  elseif not tb.state.wizard_played then
    tb.state.wizard_played = true
    start_wizard()                                 -- mini-wizard: all shots lit
  end
  tb.timer(1500, function() tb.kick("scoop", 4.0, 75) end)
end)
-- each mode's completion path: tb.state.done[mode] = true
--                              tb.backglass.animate("mode_complete")
```

### 5.5 Multiball with jackpot and super jackpot

```lua
-- Lock 3 balls to start; jackpots on the ramp; super after 3 jackpots.
-- The sim captures unconditionally and there is no confirm API (§3.4), so
-- the FIRST job of every ball_lock handler is the unlit-lock check.
tb.on("ball_start", function()
  tb.state.lock_lit = tb.state.lock_lit or false
end)
tb.on("bank_complete", function(ev)
  if ev.bank_id == "bank_gears" then                -- qualify the lock
    tb.state.lock_lit = true; tb.light_on("light_lock")
  end
end)
tb.on("ball_lock", function(ev)
  if not tb.state.lock_lit then                     -- MANDATORY (§3.4)
    tb.release_lock(ev.lock_id, 1)                  -- hand it straight back
    return                                          -- ...or the 3000 ms sim
  end                                               --    failsafe does it
  tb.state.lock_lit = false                         -- one lock per qualify
  tb.light_off("light_lock")
  tb.show_message("BALL " .. ev.count .. " LOCKED", { style = "mode" })
  if ev.count >= 3 then
    tb.backglass.animate("multiball_intro")
    tb.release_lock("lock_main", 3)     -- framework fires multiball_start
  end
end)
tb.on("multiball_start", function()
  tb.state.jackpots = 0
  tb.ball_save(10000, 3)                -- multi-use multiball ball save
  tb.light_blink("light_ramp_jackpot", "strobe")
end)
tb.on("ramp_made", function(ev)
  if ev.id == "ramp_main" and tb.state.jackpots ~= nil then
    tb.state.jackpots = tb.state.jackpots + 1
    if tb.state.jackpots < 3 then
      tb.score(100000); tb.backglass.animate("jackpot")
    else
      tb.score(500000); tb.backglass.animate("super_jackpot")
      tb.state.jackpots = 0             -- ladder restarts
    end
  end
end)
tb.on("multiball_end", function()
  tb.state.jackpots = nil
  tb.light_off("light_ramp_jackpot")
end)
```

### 5.6 Ball save grace period

```lua
-- Visible save 8 s, plus a hidden 2 s grace so edge drains feel saved.
-- The guard is essential: re-arming unconditionally on ball_save_expired
-- would loop forever (each grace window expires and re-arms the next).
tb.on("ball_start", function()
  tb.state.grace_used = false
  tb.ball_save(8000)
end)
tb.on("ball_save_expired", function()
  if not tb.state.grace_used then
    tb.state.grace_used = true
    tb.ball_save(2000)     -- silent one-time grace; then it truly ends
  end
end)
```

### 5.7 Lane change on flipper buttons

```lua
-- Flipper buttons rotate lit top lanes; complete both for bonus X (§3.1).
local ORDER = { "lane_a", "lane_b" }               -- explicit order (§1.4)
local function redraw()
  for _, id in ipairs(ORDER) do
    (tb.state.lanes[id] and tb.light_on or tb.light_off)("light_" .. id)
  end
end
tb.on("ball_start", function()
  tb.state.lanes = tb.state.lanes or { lane_a = false, lane_b = false }
  redraw()
end)
tb.on("switch_hit", function(ev)
  if ev.id == "button_flipper_left"
     or ev.id == "button_flipper_right" then       -- flippers only, §4.1
    tb.state.lanes.lane_a, tb.state.lanes.lane_b =
      tb.state.lanes.lane_b, tb.state.lanes.lane_a -- rotate lit lanes
    redraw()
  end
end)
tb.on("rollover", function(ev)
  if tb.state.lanes[ev.id] == false then
    tb.state.lanes[ev.id] = true
    tb.score(2500); redraw()
    if tb.state.lanes.lane_a and tb.state.lanes.lane_b then
      tb.state.bx = math.min((tb.state.bx or 1) + 1, 10)
      tb.set_multiplier(tb.state.bx)
      tb.state.lanes = { lane_a = false, lane_b = false }; redraw()
      tb.show_message("BONUS " .. tb.state.bx .. "X", { style = "mode" })
    end
  end
end)
```

### 5.8 Mystery award (deterministic)

```lua
-- Weighted mystery: draw exactly once, at award time (§2.6 draw order).
local MYSTERY = {                                  -- {weight, points, label}
  { 50, 10000,  "10,000" },
  { 30, 25000,  "25,000" },
  { 15, 100000, "100,000" },
  {  5, 0,      "BALL SAVE" },                     -- points 0 => special
}
tb.on("kicker_enter", function(ev)
  if ev.id ~= "saucer_mystery" then return end
  tb.kick_hold("saucer_mystery")
  local total = 0
  for _, e in ipairs(MYSTERY) do total = total + e[1] end
  local roll, acc = tb.rng_range(1, total), 0
  for _, e in ipairs(MYSTERY) do
    acc = acc + e[1]
    if roll <= acc then
      if e[2] > 0 then tb.score(e[2]) else tb.ball_save(15000) end
      tb.show_message("MYSTERY: " .. e[3], { style = "jackpot" })
      break
    end
  end
  tb.timer(1500, function() tb.kick("saucer_mystery", 3.5, 105) end)
end)
```

### 5.9 End-of-ball bonus sequence

```lua
-- Accumulate bonus in play; the framework counts it after ball_end (§3.1).
tb.on("switch_hit", function(ev)
  if ev.id == "pop_main" then tb.add_bonus(100) end
end)
tb.on("target_down", function() tb.add_bonus(500) end)
tb.on("ball_end", function(ev)
  -- Framework is about to count ev.bonus × ev.bonus_multiplier. Dress it up:
  tb.play_sound("sfx_bonus_count")
  tb.backglass.set_layout("celebration")
  tb.show_message(("BONUS %d x%d"):format(ev.bonus, ev.bonus_multiplier),
                  { style = "mode", duration_ms = 3000 })
end)
tb.on("ball_start", function()
  tb.backglass.set_layout("scores")               -- back to normal
end)
```

## 6. Complete rules.lua for test-lab

Reference rules for `test-lab`. Element ids match the test-lab `table.json`
in 09-table-format.md §7 exactly: pop bumper `pop_main`, standups
`target_left`/`target_right`, top rollover `top_lane`, lights `light_pop`/
`light_top_lane`, plus the prefab-expanded `slings_left_sling`/
`slings_right_sling` and `flippers_left_flipper`/`flippers_right_flipper`.
It implements the table's rules card ("Hit both targets to light the top
lane. Top lane scores 5000.") and references no audio ids, so it stays
valid whatever the minimal `audio.json` (12-audio.md) contains. This exact
file ships at `tables/test-lab/rules.lua` and is exercised by the M9 tests.

```lua
-- tables/test-lab/rules.lua
-- Reference rules for the test-lab table. Demonstrates: CONFIG block,
-- scoring, bonus, ball save, per-player state, one timed mode, timers.
-- Rules card: hit both targets to light the top lane; the lit top lane
-- scores 5000 (and starts Lab Frenzy).

-- CONFIG: every tunable number lives here (style guide, 10 §7). ----------
local CONFIG = {
  SCORE_SLING      = 110,    -- per slingshot fire
  SCORE_POP        = 500,    -- per pop bumper hit
  SCORE_TARGET     = 1000,   -- per standup target hit
  SCORE_LANE       = 500,    -- top lane, unlit
  SCORE_LANE_LIT   = 5000,   -- top lane, lit (rules-card award)
  SCORE_FRENZY_HIT = 250,    -- added to every switch during Lab Frenzy
  BONUS_POP        = 100,    -- end-of-ball bonus per pop hit
  BONUS_TARGET     = 500,    -- end-of-ball bonus per target hit
  FRENZY_MS        = 20000,  -- Lab Frenzy duration (live play)
  BALL_SAVE_MS     = 8000,   -- ball save window per ball
}

-- Helpers ----------------------------------------------------------------
local function targets_done()
  return tb.state.targets.left and tb.state.targets.right
end

local function set_lane_light()
  if tb.state.lane_lit then
    tb.light_blink("light_top_lane", "fast_blink")
  else
    tb.light_off("light_top_lane")
  end
end

local function start_frenzy()
  tb.state.frenzy = true
  tb.light_blink("light_pop", "strobe")         -- pop light = frenzy tell
  tb.backglass.set_layout("mode")
  tb.backglass.animate("mode_start")
  tb.show_message("LAB FRENZY!", { style = "mode", duration_ms = 3000 })
  tb.timer(CONFIG.FRENZY_MS, function()         -- frozen while not in play
    tb.state.frenzy = false
    tb.light_off("light_pop")
    tb.backglass.set_layout("scores")
    tb.show_message("FRENZY OVER", { style = "info" })
  end)
end

-- Lifecycle --------------------------------------------------------------
function on_init()
  tb.light_off("light_pop")     -- explicit start state; obvious on reload
  tb.light_off("light_top_lane")
end

tb.on("ball_start", function()
  -- Per-player progress persists across balls; init once per player.
  tb.state.targets = tb.state.targets or { left = false, right = false }
  tb.state.lane_lit = tb.state.lane_lit or false
  tb.state.frenzy = false                       -- frenzy never spans balls
  tb.light_off("light_pop")
  set_lane_light()
  tb.ball_save(CONFIG.BALL_SAVE_MS)
end)

tb.on("game_end", function(ev)
  tb.show_message("PLAYER " .. ev.winner .. " WINS", { style = "jackpot" })
end)

-- Scoring ----------------------------------------------------------------
tb.on("switch_hit", function(ev)
  if ev.tags[1] == "button" then return end     -- buttons never score (§4.1)
  -- Frenzy rides on top of all normal scoring below.
  if tb.state.frenzy then tb.score(CONFIG.SCORE_FRENZY_HIT) end

  if ev.id == "slings_left_sling" or ev.id == "slings_right_sling" then
    tb.score(CONFIG.SCORE_SLING)
  elseif ev.id == "pop_main" then
    tb.score(CONFIG.SCORE_POP)
    tb.add_bonus(CONFIG.BONUS_POP)
  elseif ev.id == "target_left" or ev.id == "target_right" then
    local side = (ev.id == "target_left") and "left" or "right"
    tb.score(CONFIG.SCORE_TARGET)
    tb.add_bonus(CONFIG.BONUS_TARGET)
    if not tb.state.targets[side] then
      tb.state.targets[side] = true
      if targets_done() and not tb.state.lane_lit then
        tb.state.lane_lit = true
        set_lane_light()
        tb.show_message("TOP LANE LIT", { style = "info" })
      end
    end
  end
end)

tb.on("rollover", function(ev)
  if ev.id ~= "top_lane" then return end
  if tb.state.lane_lit then
    tb.state.lane_lit = false
    set_lane_light()
    tb.state.targets = { left = false, right = false }  -- re-arm the ladder
    tb.score(CONFIG.SCORE_LANE_LIT)
    if not tb.state.frenzy then start_frenzy() end
  else
    tb.score(CONFIG.SCORE_LANE)
  end
end)

-- Feedback-only handlers -------------------------------------------------
tb.on("tilt_warning", function(ev)
  tb.show_message("WARNING " .. ev.count, { style = "warning" })
end)

tb.on("tilt", function()
  tb.backglass.animate("tilt")   -- framework already killed the flippers
end)
```

## 7. Style guide for rules files

Table-authoring LLMs and humans must follow these rules; `tb_validate`
warns on violations where statically checkable.

1. **CONFIG first.** One `local CONFIG = { ... }` table at the top holds
   every tunable number (scores, durations, speeds, angles): `UPPER_SNAKE`
   keys, one comment per line with units. No numeric literal with gameplay
   meaning outside CONFIG (structural literals like array indices are fine).
   Tuning is a one-place edit.
2. **Naming.** `snake_case` locals and functions; element/light/sound id
   strings exactly as in table.json / audio.json. Prefix `tb.state`
   booleans with the feature (`tb.state.frenzy`, not `tb.state.active`).
3. **Structure**, in file order: CONFIG → helpers → lifecycle handlers →
   one section per feature, each with a banner comment. Prefer one
   `switch_hit` router per file over many competing `switch_hit` handlers.
4. **State discipline.** Cross-event state lives in `tb.state` (per-player
   for free). File-level `local` variables only for values genuinely global
   to the machine (e.g. a timer id cancelable from another handler) — they
   are *not* swapped per player.
5. **Determinism habits.** `ipairs` or explicit ordered lists for anything
   gameplay-visible (§1.4); draw from `tb.rng` only at the moment of use.
6. **No busy logic.** Never poll in `timer_tick` for something an event
   already reports. More than ~50 live timers at once is a design smell.

## Common pitfalls

1. **Calling Lua unprotected.** Plain `sol::function` calls abort the
   process on error. Every entry — top level, `on_init`, handlers, timer
   callbacks — must be a `sol::protected_function` call with the §2.5 path.
2. **Watchdog hook missing on coroutines.** Lua 5.4 coroutine threads do not
   inherit debug hooks. Wrap `coroutine.create`/`wrap` (§1.2) or an infinite
   loop inside a coroutine runs forever on the sim thread.
3. **Nondeterministic `pairs()`.** Stock Lua seeds string hashes from the
   clock. Build with the fixed `luai_makeseed` of §1.4 and replay-test a
   rules file that iterates tables.
4. **Running script work off the sim thread** (e.g. dispatching from the
   render loop). All script execution happens in phases 2–4 of the tick
   pipeline (§2.2), nowhere else.
5. **Applying physical actions mid-tick.** `tb.kick` etc. must latch to the
   next tick's phase 1 (§2.2); mutating physics during dispatch makes
   handler order affect trajectories and breaks replays intermittently.
6. **Timers in float seconds.** Timers are integer tick counters (1 tick =
   1 ms); floats drift and break the freeze rule (§3.6). Also: forgetting to
   cancel all timers after `ball_end` leaks mode timers into the next
   player's ball.
7. **Forgetting the `tb.state` swap.** `tb.state` must proxy the *current*
   player's table, swapped before `player_up` handlers run. One-player
   testing hides this bug; test with 2+.
8. **A script error killing the tick.** An exception escaping into the sim
   loop crashes or corrupts; an error in one handler must not prevent later
   handlers (§2.5).
9. **Breaking the pair-firing rule.** `switch_hit` first, then the
   specialized event, same tick (§4). Frenzy modes rely on it. The one
   documented exception is `captive_full_travel`, which reports the
   captive's *arrival* and lands tens of ticks later (§4.1) — do not
   "fix" it by forcing it into the strike tick.
10. **Full GC on the sim thread.** A full collection costs more than a whole
    tick. Incremental steps only (§1.1); the wrapped `collectgarbage` must
    not allow `"collect"`.
11. **Holding a ball with no release path.** The `capture_ms` auto-eject
    (§3.4) is mandatory sim behavior precisely so a rules bug cannot strand
    a ball; `tb.kick_hold` opts out and makes the script responsible. Every
    hold needs a guaranteed `tb.kick` path (timer, event, or button). A
    missing one is **not** a hang: with no FREE ball and none in the plunger
    lane for 30,000 ticks (30 s), the framework runs a ball search that
    *does* eject kickers and locks, and logs an **error**
    (11-game-framework.md §4.6) — the ball is recovered and counted. It is
    still an author bug: 30 s of dead table, and the error line alone makes
    `tb_autoplay --check-bounds` exit 1 (14-authoring-guide.md §8.2).
12. **Treating clamps as errors.** Out-of-range numeric values clamp with a
    warn (§3); only type errors raise. Scripts keep running on
    sloppy-but-typed input.
13. **Waiting for a "confirm the lock" call.** There is none (§3.4): the sim
    captures unconditionally. An unlit lock must call
    `tb.release_lock(id, 1)` from its own `ball_lock` handler (§5.5), or the
    3000 ms sim failsafe hands the ball back with a warning — a stall the
    player reads as a bug.
14. **Enumerating the canon §5.7 surface as functions only.** Four of the
    names canon lists are tables, not callables: `tb.state`, `tb.game`,
    `tb.table_info` (§3.8) and `tb.backglass` (§3.7). A conformance check
    that demands `type(tb.<name>) == "function"` for the whole list — or a
    binding that exposes `tb.game()`/`tb.table_info()` as getters to satisfy
    such a check — contradicts §3.8 and fails `Api.EveryCanonNameExists`
    (04-milestones.md M9) for the wrong reason.
15. **Deferring work out of the `ball_end` handler.** Framework events are
    dispatched synchronously inside the FSM step (§2.2 phase 3), so
    `tb.add_bonus` called *in* the handler still counts; a `tb.timer`
    scheduled from it never fires (all timers are canceled right after
    `ball_end`, §3.6) and posts made later are discarded by the frozen
    ledger (11-game-framework.md §4.5).
16. **Treating the instruction budget as roomy.** 10,000 instructions is
    ~100 µs of a 1 ms tick, shared by every handler and timer callback in
    that tick (§2.4). Per-frame-shaped work — rescanning all elements,
    rebuilding tables, string formatting in a hot handler — exhausts it and
    permanently disables whichever handler happens to hit zero. Keep
    handlers event-shaped and cache derived values in `tb.state`.

## Done when

- [ ] Sandbox: from a test rules file, `io`, `os`, `require`, `package`,
      `debug`, `load`, `dofile`, `loadstring`, `loadfile`, `string.dump`
      are all `nil`; `math.random()` / `math.randomseed()` raise the §1.2
      message; `collectgarbage("collect")` raises, `("count")` works.
- [ ] Every §3 function has unit tests covering documented behavior,
      type-error raise, and out-of-range clamp / unknown-id warn paths.
- [ ] Audio surface agrees with 12-audio.md: `tb.play_sound(id, {duck=true})`
      sets bit0 of the emitted `SoundEvent` flags and a plain call does not;
      `tb.play_music` crossfades over 100 ms, no-ops on the already-playing
      id, and accepts the §9 reserved ids (`attract`, `main`, `mode`,
      `multiball`, `wizard`, `game_over`).
- [ ] Watchdog: a handler with `while true do end` is aborted and disabled
      within its first tick, after at most 10,000 + one hook interval of
      instructions — measured script time for that tick ≤ ~110 µs, i.e.
      inside ADR-006's < 100 µs criterion plus the hook granularity (§2.4);
      a handler with a 40,000-iteration loop trips the same path rather
      than running to completion; subsequent sim ticks carry no script cost
      and complete in < 1 ms wall time; the same holds for the loop inside
      a coroutine.
- [ ] Error path: a handler calling `error("x")` logs once (rate-limited
      after), later handlers for the same event still run, and 10
      consecutive failures disable the handler with a distinct log line.
- [ ] Determinism: `tb_autoplay` replay of a recorded input stream over
      `test-lab` yields byte-identical event/action logs across 5 runs,
      including a rules file using `pairs`, `tb.rng`, coroutines, timers.
- [ ] Timers: a 5000 ms timer fires exactly 5000 `BALL_IN_PLAY` ticks after
      creation; it does not advance during bonus count, between balls, or
      tilt; all timers cancel after `ball_end`; `tb.cancel_timer` on
      fired/unknown ids is a no-op.
- [ ] Events: an instrumented table fires every §4 event with exactly the
      documented payload fields (none missing, none extra), `switch_hit`
      preceding specialized events, cabinet buttons (flippers and launch)
      reported per §4.1.
- [ ] Tick phases (§2.2): a probe rules file logging (tick, phase) per
      callback shows sim events (`switch_hit`…`captive_full_travel`,
      `ball_launched`, `drain`) in phase 2, framework events (`ball_start`,
      `ball_end`, `player_up`, `game_end`, `tilt*`, `multiball_*`,
      `ball_save_expired`, `timer_tick`) in phase 3, timer callbacks in
      phase 4 — and a `ball_end` handler calling `tb.add_bonus(1000)`
      changes the total the framework then counts, in that same tick
      (11-game-framework.md §4.5).
- [ ] Ball lock (§3.4): a table with **no** `ball_lock` handler gets its
      captured ball back 3000 ms later with a logged warning; the §5.5
      unlit-lock handler returns an unlit capture within one 500 ms release
      cadence and logs no failsafe warning; no API exists to confirm or
      refuse a capture.
- [ ] Captive ball (§4.1): on `shaker` geometry an in-line 2.0 m/s strike
      fires `switch_hit` with `speed` = the striking ball's impact speed,
      then `captive_full_travel{id}` ~28 ticks later; a strike below that
      element's computed 0.51 m/s threshold fires `switch_hit` only, with no
      payload flag distinguishing the two cases.
- [ ] Tilt (§3.4, §3.6): a ball held via `tb.kick_hold` and balls sitting in
      a `ball_lock` are all ejected at the element defaults on `tilt`, even
      though script timers are frozen and button `switch_hit`s are
      suppressed; the same holds on Duel timeout.
- [ ] `tb.state` swaps correctly in a 2-player game (independent progress,
      persists across each player's balls, cleared at game end); `tb.game`
      and `tb.table_info` expose exactly the §3.8 fields and any write to
      either raises; `Api.EveryCanonNameExists` (04-milestones.md M9) finds
      every canon §5.7 name with nothing missing and nothing extra, treating
      `tb.state`, `tb.game`, `tb.table_info` and `tb.backglass` as tables and
      the rest as functions (§3.8).
- [ ] Hot reload: with `--dev`, saving `rules.lua` restarts the game with
      new rules within 1 s; a syntax error keeps old rules and shows the
      error; without `--dev` no file watching occurs.
- [ ] `tables/test-lab/rules.lua` matches §6, passes `tb_validate`, and a
      scripted `tb_autoplay` session scores via slings, pop, targets, and
      the top lane, lights the lane by completing both targets, triggers
      Lab Frenzy, uses ball save, and completes a full game.
