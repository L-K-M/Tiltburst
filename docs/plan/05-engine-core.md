# 05 — Engine Core

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 01-product.md (R1, R2), 02-decisions.md (ADR-005: SDL3 GPU),
03-process.md. Consumed by every later doc; 06-rendering.md, 07-displays.md,
08-physics.md, 10-scripting.md, 12-audio.md and 16-testing-ci.md build
directly on the contracts defined here.

This document owns: application lifecycle, the main/sim thread loops, timing
and pacing, the input system (all raw paths), the snapshot and event-ring
transport between threads, configuration files, logging, the RNG, and latency
instrumentation. Threading is exactly canon §5.4: **main**, **sim**,
**audio**, **raw-input** — no other threads exist in v1.

## 1. Bootstrap sequence

`tiltburst` main() must execute these steps in this exact order. Any step
that fails hard (marked ✗) logs at `error`, flushes the log, and exits with
code 1. Steps marked (fallback) degrade and continue.

1. **Parse CLI** (§2). Pure string parsing, no SDL calls. Unknown flag or
   malformed value: print usage to stderr, exit code 2 (exit-code contract
   in §2 — CI reads 2 as *skip*, never as failure).
2. **Init logger** with the in-memory ring only (§12). File sink attaches in
   step 5. Install a terminate handler and (POSIX) SIGSEGV/SIGABRT handlers
   whose only action is flushing the log ring to disk and re-raising.
3. **SDL metadata + init** ✗:
   ```cpp
   SDL_SetAppMetadata("Tiltburst", TB_VERSION_STRING, "com.tiltburst.tiltburst");
   SDL_Init(headless ? SDL_INIT_EVENTS
                     : SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
   ```
   `--headless` must never initialize video, GPU, or audio.
4. **Pref path** ✗: `char* p = SDL_GetPrefPath("tiltburst", "tiltburst");`
   Store as `tb::paths::pref()`. Create subdirectory `logs/` under it.
5. **Attach log file sink**: `logs/tiltburst-YYYYMMDD-HHMMSS.log` (local
   time at launch). Delete oldest files so at most 5 logs remain. Log one
   `info` line: version, git hash, build config, OS, CPU count.
6. **Load `settings.json`** (§11) from pref path (fallback: defaults). A
   parse error renames the bad file to `settings.json.bad`, logs `warn`,
   and continues with defaults.
7. **Load `displays.json`** (or the `--display-config <path>` override)
   and run display detection — algorithm in 07-displays.md. (Skipped in
   `--headless`.)
8. **Create windows** per the display assignment (07-displays.md §7).
9. **Create GPU device** ✗ with the shader formats **actually present on
   disk** — never a hardcoded SPIRV|DXIL|MSL mask. `available_shader_formats()`
   is the runtime scan of `shaders/` owned by 06-rendering.md §16.4 and
   called exactly as 06 §3 shows: the formats whose blobs installed,
   intersected with the three SDL knows. DXIL needs DXC + `dxil.dll`
   (Windows CI only), so on a build where DXIL was never produced the mask
   omits it and SDL picks a backend whose blobs exist (e.g. Vulkan on
   Windows) instead of a D3D12 device with nothing to load:
   ```cpp
   SDL_GPUDevice* dev = SDL_CreateGPUDevice(
       available_shader_formats(),   // 06 §16.4: on-disk formats ∩ SPIRV|DXIL|MSL
       /*debug_mode=*/ cli.dev || TB_DEV_BUILD, /*name=*/ nullptr);
   SDL_SetGPUAllowedFramesInFlight(dev, 1);   // device-wide; R2 latency cap
   ```
   `debug_mode` is `cli.dev || TB_DEV_BUILD` — **the identical expression
   06 §3 passes**, and the two sites must never drift apart. `TB_DEV_BUILD`
   (CMake option `TB_DEV`) is true in Debug/RelWithDebInfo and false in a
   shipping Release, so the SDL GPU debug layer is on in every dev build
   without a flag; `--dev` (§2) additionally forces it on at runtime, which
   is the only way to get the validation layer out of a shipping Release
   binary.

   Then claim each window: `SDL_ClaimWindowForGPUDevice(dev, win)` and set
   swapchain parameters (present mode selection in 07-displays.md §7).
   A scan that finds no blobs at all leaves no usable backend and device
   creation fails here like any other ✗ step — exit 1 in a normal run, exit 2
   under `--render-smoke` (the deliberate asymmetry in §2.1).
10. **Init subsystems**: renderer (06), audio device + mixer (12), input
    sources (§9.8 selection policy), table loader (09).
11. **Load table**: `--table <slug>` if given, else `settings.last_table`,
    else `neon-drift`. Missing table ✗.
12. **Start threads**: audio (12-audio.md), raw-input (§9), then sim (§6).
13. **Enter main loop** (§5).

**Shutdown** (window close, `quit` from menu, or SIGINT) is the exact
reverse: set `g_quit` (atomic), join sim thread, stop audio, stop input
sources, `SDL_WaitForGPUIdle`, destroy renderer, release windows from the
device, destroy device, destroy windows, save `settings.json` (§11.2),
flush log, `SDL_Quit()`. Exit code 0.

## 2. Command-line interface

All flags are long-form only, `--flag value` or `--flag=value`.

| Flag | Argument | Effect |
|---|---|---|
| `--version` | — | Print `tiltburst <version>` (`TB_VERSION_STRING`, e.g. `tiltburst 0.1.0`) to **stdout** and **exit 0**, inside §1 step 1 — before SDL init, the pref path, or any window. It short-circuits the rest of the command line, so it exits 0 whatever else was passed. The only flag M0 implements (04-milestones.md M0, whose acceptance is exactly this line), and the process-launch probe 16-testing-ci.md §2.10 spawns for `perf_startup.cold_boot_to_attract`. |
| `--windowed` | `WxH` e.g. `540x960` | Two resizable windows instead of fullscreen (07-displays.md §11). `W`,`H` are the playfield client size in pixels. |
| `--table` | slug | Load `tables/<slug>/` instead of last-played. |
| `--headless` | — | No video/GPU/audio. Sim + game logic only. Used by tests and `tb_autoplay`. |
| `--record` | file path | Record the consumed input stream to `<file>` (`.tbreplay`, §13). |
| `--replay` | file path | Drive the sim from a recorded `.tbreplay` stream, or a `.replay.json` test tape (§13.1); live input ignored except Escape/quit. Unpaced (as fast as possible) when combined with `--headless` or `--screenshot`, real-time paced otherwise. |
| `--screenshot` | `out.png` | Render one offscreen frame (1080×1920, rotation 0) at the time given by `--at-seconds`, write PNG (stb_image_write), exit 0. Requires GPU but creates no window. |
| `--at-seconds` | float ≥ 0 | Sim time at which `--screenshot` fires. Sim runs unpaced to that tick (`tick = round(n * 1000)`). |
| `--seed` | uint64 | Fixed RNG seed (§8.3). Without it, a fresh seed per game from `std::random_device` on the main thread (the sim itself never reads entropy or wall clock — canon §5.3). |
| `--display-config` | path | Use this file instead of pref-path `displays.json`. |
| `--dev` | — | Developer mode: log level `debug`, GPU debug layer forced on, table hot-reload watcher on (09-table-format.md), Lua error overlay (10-scripting.md). The debug layer is already on in dev builds — §1 step 9 and 06 §3 both pass `cli.dev \|\| TB_DEV_BUILD` — so this flag only adds it where `TB_DEV_BUILD` is false, i.e. in a shipping Release. |
| `--latency-test` | — | Photodiode test mode (§14.4). |
| `--audio-null` | — | Force the miniaudio **null backend** (silent output); also selected automatically when no audio device exists. Used by CLI tools and CI (12-audio.md §2). |
| `--audio-latency-test` | — | Audio latency measurement mode: logs `p50/p95` flipper-press→DAC estimates per 100 flips (12-audio.md §12). |
| `--latency-loopback` | — | Duplex loopback measurement over a physical line-out→line-in cable; prints button→ear latency (12-audio.md §12). |
| `--render-smoke` | — | CI smoke run: create the GPU device with **no window**, render `--frames` frames of `test-lab` offscreen, write the final frame as PNG into `--screenshot-dir`, exit 0 (exit 2 = no usable GPU backend) (16-testing-ci.md §5). |
| `--frames` | int > 0 | Frame count for `--render-smoke`. |
| `--screenshot-dir` | dir path | Output directory for `--render-smoke`. |

### 2.1 Argument errors and exit codes (binding)

**Any** flag the parser does not recognize, **any** malformed or
out-of-range value, and **any** combination that makes no sense
(`--headless --screenshot`, `--windowed --headless`, `--render-smoke`
without `--screenshot-dir`, `--frames` ≤ 0, `--at-seconds` without
`--screenshot`) prints a one-line reason **plus the full usage block to
stderr** and exits **2**. Never exit 1 for an argument problem, never
silently ignore an unknown flag, never fall back to a default value.

`--version` is the single short-circuit: it is recognized first, prints one
line to stdout and exits **0** without validating the rest of the command
line (§2). Nothing else escapes the rule above.

Exit-code contract for the `tiltburst` binary (04-milestones.md M0 and the
CI renderer-smoke step in 16-testing-ci.md §5 depend on it; 2 means *skip*,
so a mistyped flag must never look like a failing test):

| Code | Meaning |
|---|---|
| 0 | The requested run completed (normal quit, `--version` printed, `--screenshot` written, `--render-smoke` frames rendered). |
| 1 | The run started but hit a hard failure: a ✗ step of §1 (SDL init, pref path, GPU device in a normal windowed/fullscreen run), missing table, replay/tape header mismatch (§13, §13.1). |
| 2 | The requested configuration cannot run here: bad/unknown flag, malformed value, contradictory combination, **or** `--render-smoke` finding no usable GPU backend (16 §5). |
| other | Crash — CI fails the job. |

The GPU asymmetry is deliberate: in a normal run a failed
`SDL_CreateGPUDevice` is fatal (§1 step 9 → exit 1); under `--render-smoke`
the identical failure exits 2 so CI logs a skip and continues. Every exit-2
message goes to **stderr** (stdout stays reserved for `--render-smoke`
per-frame timing and tool output).

## 3. Timing and clocks

- One timebase everywhere: `tb::now_ns()` returns `SDL_GetTicksNS()`
  (monotonic nanoseconds since `SDL_Init`, callable from any thread). Every
  timestamp in this plan — input edges, snapshot publish, latency stages —
  is in this timebase. Never call `SDL_GetTicks()` (ms) on any hot path.
- The sim consumes time only as tick counts (`dt = 0.001 s` exactly, canon
  §5.3). `tb::now_ns()` is used by the sim *loop* for pacing, never by
  simulation logic or Lua.
- Linux evdev kernel timestamps are CLOCK_MONOTONIC-since-boot; convert
  them by a constant offset captured once at startup:
  `offset_ns = tb::now_ns() - clock_gettime(CLOCK_MONOTONIC)` (§9.6).
- Windows raw-input events are stamped with `tb::now_ns()` at the moment
  `WM_INPUT` is processed (§9.5).

## 4. Thread model and data flow

Canon §5.4 verbatim applies. Ownership summary:

| Thread | Runs | Produces | Consumes |
|---|---|---|---|
| main | SDL event pump, playfield render (native Hz), backglass render (~30 Hz, non-blocking), menus/game UI layer | SDL input edges, focus flag | SimSnapshot, render ring, game ring |
| sim | 1000 Hz fixed-timestep physics + Lua + GameFsm, in the four phases of §6.3 | SimSnapshot, the two SimEvent rings + the SoundEvent ring, latency records | input edge rings, atomic input bitset |
| audio | miniaudio device callback (12-audio.md) | — | sound ring (`SoundEvent`, 12 §4.1) |
| raw-input | WinRawInput *or* evdev loop | raw input edges, atomic input bitset | — |

ALL GPU work happens on main (canon). The sim thread must never touch SDL
video/GPU, and `tb_sim` must not link `tb_platform`/`tb_render`/`tb_audio`
(canon §5.1).

```
 raw-input ──edges──┐                          ┌──render ring──► main (fx, particles)
 main(SDL) ──edges──┼─► [sim thread, 1000 Hz] ─┼──sound ring───► audio thread
                    │        │                 │  (SoundEvent)
                    │        │                 └──game ring────► main (scores, UI)
      atomic bitset ┘        └─► SimSnapshot (triple buffer) ──► main (draw)
```

## 5. Main-thread loop

```
while (!g_quit):
    t_frame_begin = now_ns()

    # 1. Pump SDL events (this is also the SDLInputSource producer, §9.4)
    while SDL_PollEvent(&e):
        dispatch:
            SDL_EVENT_QUIT                     -> g_quit = true
            key down/up                        -> SDLInputSource.on_key(e)
            SDL_EVENT_WINDOW_FOCUS_GAINED/LOST -> g_app_focused.store(...)
            SDL_EVENT_DISPLAY_ADDED/REMOVED    -> displays.on_hotplug(e)   # 07 §9
            window resize (windowed mode)      -> renderer.on_resize(...)
            F1 / F2 / F3 / F4                  -> toggle overlays (§14.3)
            F12                                -> renderer.request_screenshot # 06 §15.1
            B                                  -> toggle backglass overlay # 07 §10

    # 2. Take the freshest sim state (never blocks, §7)
    snap = snapshot_buffer.read()
    t_render_begin = now_ns()
    latency.note_render_begin(snap.tick, t_render_begin)     # §14.1

    # 3. Drain rings owned by main
    n = render_ring.drain(ev_buf, MAX);  fx.consume(ev_buf, n)      # 06, 13
    n = game_ring.drain(ev_buf, MAX);    game.consume(ev_buf, n)    # 11

    # 4. Game/UI layer update (menus, backglass model, score displays) — 11
    game.update_ui(now_ns())

    # 5. Playfield frame: blocking acquire respects frames-in-flight = 1
    cmd = SDL_AcquireGPUCommandBuffer(dev)
    if SDL_WaitAndAcquireGPUSwapchainTexture(cmd, playfield_win, &tex, &w, &h):
        renderer.draw_playfield(cmd, tex, snap, overlays)            # 06
        latency.note_present(snap.tick, now_ns())
        SDL_SubmitGPUCommandBuffer(cmd)
    else:
        SDL_CancelGPUCommandBuffer(cmd)   # acquire failed (minimized/occluded):
                                          # skip the frame — 06 §4.1

    # 6. Backglass at ~30 Hz cadence, NON-blocking — canon 5.4, detail 07 §8
    if now_ns() >= next_backglass_ns:
        cmd2 = SDL_AcquireGPUCommandBuffer(dev)
        if SDL_AcquireGPUSwapchainTexture(cmd2, backglass_win, &tex2, ...)
           and tex2 != NULL:
            renderer.draw_backglass(cmd2, tex2, snap, game.backglass_model())
            SDL_SubmitGPUCommandBuffer(cmd2)
            next_backglass_ns += 33'333'333          # 30 Hz
            if next_backglass_ns < now_ns() - 100ms: next_backglass_ns = now_ns()
        else:
            SDL_CancelGPUCommandBuffer(cmd2)  # not ready: skip, deadline
                                              # unchanged — 06 §4.2, 07 §8

    perf.note_frame(t_frame_begin, now_ns())                         # §14.2
```

Every acquired command buffer is submitted (after drawing) or cancelled (on
a skipped frame) **exactly once** — never both, never neither (06 §4).

Rules: the main loop has **no sleep** of its own — pacing comes from the
blocking playfield acquire (VSYNC) or runs uncapped (MAILBOX). The main loop
never waits on the sim thread and never holds any lock the sim thread takes.

## 6. Sim-thread loop

The sim thread is started with `SDL_SetCurrentThreadPriority(
SDL_THREAD_PRIORITY_TIME_CRITICAL)` (ignore failure, log `debug`). On
Windows, call `timeBeginPeriod(1)` once on this thread (and matching
`timeEndPeriod(1)` at stop).

```
dt_ns        = 1'000'000                 # exactly 1 ms
t0           = now_ns()
tick         = 0
accum_ns     = 0
last_ns      = t0

loop until g_quit:
    # --- pacing: absolute schedule, no drift (§6.2) ---
    target_ns = t0 + (tick + 1) * dt_ns
    wake_ns   = target_ns - SPIN_MARGIN_NS          # 300'000 ns
    if now_ns() < wake_ns:
        sleep_until(wake_ns)                        # std::this_thread::sleep_until
    while now_ns() < target_ns:
        cpu_pause()                                 # _mm_pause / __yield
    jitter.record(now_ns() - target_ns)             # §6.2

    # --- accumulator ---
    now      = now_ns()
    accum_ns += now - last_ns
    last_ns  = now
    pending  = accum_ns / dt_ns                     # integer division

    # --- overrun policy ---
    if pending > 50:                                # more than 50 ms behind
        overruns_total += 1
        dropped_ticks_total += pending - 50
        log_warn_ratelimited("sim overrun: dropped {} ticks", pending - 50)
        pending  = 50
        accum_ns = 50 * dt_ns

    # --- run ticks: the four tick phases, in this exact order (§6.3) ---
    repeat pending times:
        latch_input(tick)          # §9.2: atomic bitset + drain <=64 edges

        # phase 1 — physics integration + sim event generation
        sim.step(dt)               # 08; queues this tick's sim events

        # phase 2 — dispatch sim events to Lua, in emission order;
        #           handlers per event in registration order (10 §2.2)
        script.dispatch_sim_events(tick)

        # phase 3 — step the GameFsm (11 §1). Framework-originated events
        #           are dispatched to Lua SYNCHRONOUSLY as the FSM emits
        #           them, in emission order, inside this same tick.
        game.step(tick)

        # phase 4 — expired script timers (deadline == tick, ascending
        #           timer_id), Lua GC step, publish
        script.fire_timers(tick)                              # 10 §3.6
        script.gc_step()                                      # lua_gc STEP
        emit SimEvents -> render_ring, game_ring              # §8.2
        emit SoundEvents -> sound_ring                        # §8.2, 12 §4.1
        accum_ns -= dt_ns
        tick += 1
        if this is the last pending tick:
            publish_snapshot(tick)                  # §7 — once per iteration

    record.append_tick_edges(...) if --record       # §13
```

Normal case: `pending == 1`, snapshot published every 1 ms. During catch-up
bursts only the final tick of the burst is published (intermediate snapshots
would never be seen by a 60–240 Hz reader).

In `--replay` + unpaced mode the pacing block is skipped entirely: run
`while (more recorded ticks) { inject edges; step; }` at full speed. In
`--headless` real-time mode the pacing block runs unchanged.

### 6.1 Overrun policy rationale (binding)

50 ticks = 50 ms of debt. Clamping loses sim-time relative to wall time
(the ball appears to pause) which is strictly better than a spiral of
death. `overruns_total` and `dropped_ticks_total` appear in the F1 overlay
(§14.2). A single tick must cost < 100 µs on reference hardware (perf gate
in 16-testing-ci.md); overruns in normal play indicate a bug, not load.

### 6.2 Pacing quality (binding constants + tuning)

- `SPIN_MARGIN_NS = 300'000` (300 µs): sleep until target − margin, spin
  the rest. Rationale: OS sleep granularity is ~55 µs–1 ms depending on
  platform; 300 µs covers p99.9 oversleep on Windows with 1 ms timer
  resolution while burning < 0.3 CPU-core.
- Jitter = `wake_actual − target`, recorded per tick in a 4096-entry ring.
  Every 10 000 ticks log at `debug`: p50/p99/max in µs.
- **Acceptance test** (reference hardware, 60 s idle attract mode):
  p99 jitter ≤ 100 µs, max ≤ 500 µs, zero overruns.
- **Tuning procedure** if the acceptance test fails: raise `SPIN_MARGIN_NS`
  in 100 µs steps (upper bound 1 000 000 ns = full spin) until it passes;
  commit the new constant with a JOURNAL.md note. Do not add config for it.

### 6.3 Tick phase order (binding)

Every 1000 Hz tick executes exactly these four phases, in this order. This
is the same statement as 10-scripting.md §2.2 and 11-game-framework.md §1 —
all three documents describe one pipeline; if they ever differ, they are
wrong, not alternatives.

1. **Physics integration + sim event generation.** `sim.step(dt)`
   (08-physics.md) integrates and queues this tick's sim events. Physical
   script actions latched during the previous tick (`tb.kick`,
   `tb.kick_hold`, `tb.release_lock`, `tb.magnet_*`,
   `tb.set_flipper_enabled`, `tb.add_ball`, `tb.drop_bank_reset`,
   `tb.gate_*`) are applied at the start of this phase (10 §2.2).
2. **Dispatch sim events to Lua handlers**, in emission order; per event,
   handlers run in registration order.
3. **Step the GameFsm.** Framework-originated events — `game_start`,
   `ball_start`, `ball_end`, `player_up`, `game_end`, `tilt_warning`,
   `tilt`, `multiball_start`, `multiball_end`, `ball_save_expired`,
   `timer_tick` — are dispatched to Lua **synchronously as the FSM emits
   them, in emission order, within this same tick**. That synchronous
   dispatch is what makes 11 §4.5's "scripts may call `tb.add_bonus` inside
   the `ball_end` handler" implementable: the framework reads the bonus it
   is about to count *after* the handler returns. Never queue a framework
   event for the next tick.
4. **Fire expired script timers** (deadline == this tick, ascending
   `timer_id`), **Lua GC step, publish the snapshot** (§7) and flush the
   event rings (§8.2).

The framework's event **emission order is part of the deterministic record**
(canon §5.3), and it is gated mechanically: 16-testing-ci.md §2.4.1's
`state_hash()` carries an **event-sequence component** — a rolling hash over
the types and ids of the sim events and framework events emitted during the
tick, in emission order. So the same seed and input stream must produce the
same events in the same order within the same tick, or the hash sequence
diverges and the §2.4.3 determinism tests (`det_replay`, `det_golden`,
`det_feel`) fail. This holds for every table, not only `test-lab`.

Catch-up bursts (`pending > 1`) run phases 1–4 in full for every tick of
the burst; only the *snapshot publish* of phase 4 is coalesced onto the
final tick, because intermediate snapshots would never be seen by a
60–240 Hz reader. Timers, GC and event pushes are never skipped or batched.

## 7. SimSnapshot and the triple buffer

### 7.1 SimSnapshot contents

POD, fixed size, no pointers, safe to `memcpy`. All fields little-endian
native. Sizes chosen to cover every shipped table (canon §5.6, §5.8).

```cpp
namespace tb {

constexpr int kMaxBalls         = 6;    // single definition, in tb_sim (08 §1.2):
                                        // 4 trough balls + tb.add_ball headroom;
                                        // 09 caps playfield.ball_count at 6.
                                        // Snapshot arrays are sized by it.
constexpr int kMaxFlippers      = 6;
constexpr int kMaxLights        = 256;
constexpr int kMaxElementStates = 512;

struct BallSnap {
    float    x, y;        // m, playfield coords (canon 5.3)
    float    vx, vy;      // m/s
    float    z;           // m above playfield (ramps/layers)
    float    omega;       // rad/s spin about z (visual roll — 06)
    uint8_t  layer;       // 0 = main, 1 = upper (canon 5.6)
    uint8_t  flags;       // bit0 active, bit1 on_ramp_path, bit2 held(kicker/magnet)
    uint16_t _pad;
};

struct FlipperSnap {
    uint16_t element;     // element index in table order (09)
    uint16_t energized;   // 0/1
    float    angle;       // rad, current
    float    omega;       // rad/s
};

struct ElementVisualState {   // one per element whose visual state changed
    uint16_t element;
    uint16_t state;       // element-type-specific enum — defined in 08/09
    float    t;           // 0..1 animation parameter
};

struct LightState {       // per-light pattern state; the sim stores numeric
                          // parameters only — brightness is evaluated on the
                          // CPU each frame from snapshot.tick (06 §14.3)
    uint8_t  mode;        // 0 off, 1 on, 2 blink_square, 3 breathe_sine, 4 chase
    uint8_t  duty_pct;    // square duty, 0..100
    uint16_t rate_chz;    // rate in centihertz (200 = 2.00 Hz)
    uint32_t start_tick;  // sim tick when the pattern started
    uint8_t  chase_index, chase_len;
    uint8_t  brightness_pct;  // master, set by script (default 100)
    uint8_t  _pad;
};

struct SimSnapshot {
    uint64_t tick;
    double   sim_time_s;          // tick * 0.001 exactly
    uint64_t publish_ts_ns;       // instrumentation (§14.1)
    uint64_t last_input_ts_ns;    // newest edge consumed by this tick, 0 if none
    uint32_t ball_count;
    uint32_t flipper_count;
    uint32_t element_state_count;
    uint32_t tilt_warnings;       // current warning count — 11
    uint8_t  tilted;              // 0/1
    uint8_t  _pad[3];
    float    nudge_off_x, nudge_off_y;   // visual table bob, m — 08
    float    plunger_pull;               // 0..1
    BallSnap           balls[kMaxBalls];
    FlipperSnap        flippers[kMaxFlippers];
    ElementVisualState element_states[kMaxElementStates];
    LightState         lights[kMaxLights];  // evaluated per frame by 06 §14.3
};

}  // namespace tb
```

Total ≈ 7.4 KB; copying it 1000×/s is ≈ 7.4 MB/s — negligible. The
renderer draws the latest snapshot directly with **no interpolation**: at
1000 Hz sim the snapshot is at most ~1 ms + jitter stale, below one frame
at 240 Hz. Never add interpolation "for smoothness".

### 7.2 Triple buffer — the algorithm (binding)

Single writer (sim), single reader (main — both playfield and backglass
draws happen on main and read the same slot). Three slots; at every moment
the slot indices {0,1,2} are partitioned between: one held in `latest_`,
one owned by the writer, one owned by the reader. `exchange` swaps
ownership atomically; a `fresh` bit distinguishes "new since last read".

```cpp
template <typename T>
class TripleBuffer {
public:
    // ---- writer side (sim thread ONLY) ----
    T& write_slot() { return slots_[write_idx_]; }   // fill, then publish()
    void publish() {
        uint32_t prev = latest_.exchange(write_idx_ | kFresh,
                                         std::memory_order_acq_rel);
        write_idx_ = prev & kIndexMask;   // recycle whichever slot we displaced
    }

    // ---- reader side (main thread ONLY) ----
    // Returned reference is valid until the NEXT read() call.
    const T& read() {
        if (latest_.load(std::memory_order_relaxed) & kFresh) {
            uint32_t prev = latest_.exchange(read_idx_,
                                             std::memory_order_acq_rel);
            read_idx_ = prev & kIndexMask;
        }
        return slots_[read_idx_];
    }

private:
    static constexpr uint32_t kIndexMask = 0x3u;
    static constexpr uint32_t kFresh     = 0x4u;
    T slots_[3] {};
    std::atomic<uint32_t> latest_ {0};   // slot 0, not fresh
    uint32_t write_idx_ = 1;             // writer-owned slot
    uint32_t read_idx_  = 2;             // reader-owned slot
};
```

Memory-ordering rationale (keep this comment in the code): the writer's
`exchange` is a *release* so all slot writes are visible to a reader that
*acquires* the same value; both exchanges are `acq_rel` because each side
also takes ownership of the slot the other side surrendered and must not
reorder its slot access before the exchange. Never "optimize" these to
`relaxed`. No CAS loops, no waiting, on either side — both operations are
one unconditional atomic exchange.

## 8. Event rings

### 8.1 SimEvent

One wire format for the sim→main legs (render + game). Exactly 32 bytes.
Sounds are **not** carried as `SimEvent` — the sim→audio leg has its own
16-byte `SoundEvent` record, owned by 12-audio.md §4.1 (§8.2).

```cpp
struct SimEvent {
    uint64_t tick;        // sim tick that produced it
    uint16_t type;        // SimEventType — physics types in 08, script in 10
    uint16_t element;     // element index, 0xFFFF = none
    float    x, y;        // position in playfield meters (0,0 if n/a)
    float    a, b;        // per-type payload (impulse Ns, speed m/s, ...)
    uint32_t data;        // per-type aux (target index, score value low bits, ...)
};
static_assert(sizeof(SimEvent) == 32);
```

### 8.2 Ring topology (binding)

The **sim thread is the only producer**. There are exactly three SPSC
rings, one per consumer, but they do **not** all carry the same record
type: the two sim→main rings carry 32-byte `SimEvent`s, the sim→audio ring
carries 16-byte `SoundEvent`s.

| Ring | Element | Capacity | Consumer thread | Drained | Used for |
|---|---|---|---|---|---|
| `render_ring` | `SimEvent` (32 B) | 4096 → 128 KB | main | once per frame (loop step 3) | particles, flashes, shakes — 06/13 |
| `game_ring` | `SimEvent` (32 B) | 4096 → 128 KB | main | once per frame (loop step 3) | scores, UI, persistence — 11 |
| `sound_ring` | `SoundEvent` (16 B) | 1024 → 16 KB | audio | in the miniaudio callback — 12 §4.2 | sample-accurate SFX |

Capacities are powers of two. The sim pushes every `SimEvent` to **both**
SimEvent rings; those two consumers filter by `type`. (A per-ring type mask
is a permitted later optimization; do not build it in v1.)

**The audio leg (12-audio.md §4.1 is normative).** Sounds never cross the
sim→audio boundary as `SimEvent`. The sim emits one `SoundEvent` per sound,
at the tick the sound happens — from the engine-automatic purposes (12 §7.2)
and from `tb.play_sound` alike (Lua runs on the sim thread, §6). 12 §4.1
owns the struct, the capacity (1024) and the overflow policy; this document
only fixes where the ring sits in the topology. Crucially, purpose→patch
resolution (through the table `map`, interned at table load), `velocity` and
`pan` are computed **sim-side at emission** — deterministic, part of the
simulation — so the audio callback never consults the sound map or intern
table; it only schedules ready-made events onto the sample clock (12 §4.2).

`sound_ring` therefore does **not** use the §8.3 drop-oldest discipline: it
is the same SPSC structure, but on overflow the producer drops the **new**
event and increments a sim-side counter mirrored into the audio stats'
`dropped_events` (12 §4.1, §2.3). Drop-oldest in §8.3 applies to
`render_ring` and `game_ring` only. UI sounds from menus do not enter this
ring at all: they travel main→audio as `AudioCommand{PlayUi, patch}` (12
§4.1) and have no tick mapping.

### 8.3 Overwrite ring: drop-oldest with a torn-read guard (binding)

This is the template behind `render_ring`, `game_ring` and the input edge
rings (§9.2); the `SoundEvent` ring keeps this structure but replaces the
policy below with 12 §4.1's drop-new (§8.2).

Overflow must drop the **oldest** events (a fresh collision flash matters
more than a 4-second-old one) and count drops; the counter is shown in the
F1 overlay. Because the producer must never wait, this is an *overwrite*
ring: the producer always writes; the consumer detects being lapped.

```cpp
template <typename T, uint32_t N>          // N power of two: 4096 for the
                                           // SimEvent rings, 1024 for edges
class EventRing {
    static_assert((N & (N - 1)) == 0);
    static constexpr uint32_t kMargin = 64;  // resync slack, see below
public:
    // Producer (sim thread only). Never blocks, never fails.
    void push(const T& ev) {
        uint64_t w = write_.load(std::memory_order_relaxed);
        slots_[w & (N - 1)] = ev;
        write_.store(w + 1, std::memory_order_release);
    }

    // Consumer (its one fixed thread only). Copies up to max events.
    size_t drain(T* out, size_t max) {
        uint64_t w = write_.load(std::memory_order_acquire);
        if (w - read_ > N - kMargin) resync(w);          // lapped (or nearly)
        size_t n = 0;
        while (read_ < w && n < max) {
            out[n] = slots_[read_ & (N - 1)];
            // Torn-read guard: slot (read_ & mask) is being overwritten iff
            // the producer has started event index read_+N, i.e. write >= read_+N.
            if (write_.load(std::memory_order_acquire) - read_ >= N) {
                w = write_.load(std::memory_order_relaxed);
                resync(w);                                // discard torn copy
                continue;
            }
            ++read_; ++n;
        }
        return n;
    }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    void resync(uint64_t w) {
        uint64_t new_read = w - (N - kMargin);
        dropped_.fetch_add(new_read - read_, std::memory_order_relaxed);
        read_ = new_read;
    }
    T slots_[N];
    std::atomic<uint64_t> write_ {0};      // total events ever pushed
    uint64_t read_ = 0;                    // consumer-private
    std::atomic<uint64_t> dropped_ {0};    // read by perf overlay (main)
};
```

Why `kMargin = 64`: after a resync the consumer must not sit exactly at the
overwrite frontier or the guard would trip immediately; 64 slots of slack
means the producer would need to emit 64 events during one 32-byte copy to
tear it, which cannot happen at 1000 Hz with bounded events per tick. The
64-bit counters never wrap in practice (5.8 × 10¹¹ years at 1 kHz).

## 9. Input system

### 9.1 Actions, bitset, InputState

Fixed action indices (they appear in the record file format §13 and the
settings input map §11 — never renumber):

| Index | Action (settings key) | Default binding (SDL scancode name) | Class |
|---|---|---|---|
| 0 | `left_flipper` | `"Left Shift"` | gameplay |
| 1 | `right_flipper` | `"Right Shift"` | gameplay |
| 2 | `left_flipper_2` | `"Left Shift"` (shared) | gameplay |
| 3 | `right_flipper_2` | `"Right Shift"` (shared) | gameplay |
| 4 | `plunger` | `"Space"` (hold-to-charge, 01 §6) | gameplay |
| 5 | `nudge_left` | `"Z"` | gameplay |
| 6 | `nudge_right` | `"/"` | gameplay |
| 7 | `nudge_up` | `"X"` | gameplay |
| 8 | `start` | `"1"` and `"Return"` | gameplay |
| 9 | `pause` | `"Escape"` | UI (SDL only) |
| 10–15 | `ui_up/down/left/right/select/back` | arrows, `"Return"`+`"Space"` (select), `"Escape"` | UI (SDL only, not remappable) |

One physical key may map to several actions (Left Shift drives both left
flipper actions by default; tables choose which flipper listens to which
action — 09-table-format.md). Nudge is **digital**: three keys, edges
only; the impulse shaping (magnitude via `input.nudge_level` §11.1,
direction, cooldown, tilt-bob dynamics) lives entirely in 08-physics.md.
Tilt accounting lives in 11-game-framework.md.

```cpp
struct InputEdge {
    uint64_t ts_ns;      // tb::now_ns() timebase (§3)
    uint16_t action;     // index above
    uint8_t  pressed;    // 1 press, 0 release
    uint8_t  source;     // 0 SDL, 1 WinRaw, 2 evdev, 3 synthetic (§9.2), 4 replay
};

struct InputState {                       // sim-owned, rebuilt every tick
    uint32_t buttons;                     // bit i = action i held
    uint64_t last_press_ns[16];
    uint64_t last_release_ns[16];
};
```

Producers additionally maintain a shared `std::atomic<uint32_t>
g_button_bits` — the "atomic latest-state" of canon §5.4 — updated with
`fetch_or`/`fetch_and` (release) on every edge.

### 9.2 Edge queue contract (binding)

Transport: one `EventRing<InputEdge, 1024>` per producer (SDL source, raw
source). The contract the rest of the engine relies on:

1. Every edge pushed while the ring holds < 1024 pending edges is delivered
   **exactly once, in order**, to exactly one tick. 1024 pending edges
   cannot arise from human input; if the drop counter ever increments, log
   `warn` — it indicates a harness bug.
2. `latch_input(tick)` (sim, immediately before the physics step) drains
   **at most 64 edges per tick** across all rings; the surplus stays queued
   for the next tick, never dropped. 64/ms is far beyond human rates; the
   bound keeps tick cost constant.
3. A press and release that both occur between two ticks (a sub-millisecond
   tap) arrive **in order in the same tick**; the flipper model receives
   both edges and applies its minimum-pulse rule (08-physics.md). This is
   the "no press/release is ever lost" guarantee.
4. After draining, sim latches `bits = g_button_bits.load(acquire)` and
   reconciles: any action whose level differs from the edge-reconstructed
   state gets a **synthetic edge** (`source = 3`, `ts_ns = latch time`) so
   level and edge views never diverge (covers a producer that missed an
   edge, and focus-regain resync, §9.9).
5. Edge timestamps are for latency measurement and sub-tick flipper timing
   only; live-play tick assignment is simply "the tick that drained it".
   Replay (§13) re-injects edges at recorded ticks, bypassing rings.

### 9.3 InputSource interface

```cpp
class InputSource {
public:
    virtual ~InputSource() = default;
    virtual bool start() = 0;              // spawn thread / register; false = unavailable
    virtual void stop()  = 0;              // join/unregister; idempotent
    virtual const char* name() const = 0;  // "sdl", "winraw", "evdev"
    // Sim thread calls this inside latch_input(); drains this source's ring.
    virtual size_t poll_edges(InputEdge* out, size_t max) = 0;
    // True while this source is delivering gameplay actions (§9.8).
    virtual bool active() const = 0;
};
```

All sources map physical keys → actions through the same resolved keymap
(settings §11, `SDL_GetScancodeFromName`). The keymap is rebuilt on
settings change and swapped in with an atomic pointer; sources read it
lock-free.

### 9.4 SDLInputSource (always present, all platforms)

Producer code runs inside the main-thread event pump (loop step 1):
`SDL_EVENT_KEY_DOWN` with `repeat == true` is ignored; otherwise map
`event.key.scancode` → actions, push one edge per mapped action with
`ts_ns = event.key.timestamp` (SDL3 key timestamps are already
`SDL_GetTicksNS`-based). SDL events also drive menus directly (UI-class
actions never go through the sim). This source is the fallback for
gameplay on all platforms and the only source on macOS in v1 (canon §5.4).

### 9.5 WinRawInputSource (Windows, raw-input thread)

Dedicated thread = the canon **raw-input** thread. Setup:

```cpp
// On the raw-input thread:
WNDCLASSEXW wc { .cbSize = sizeof(wc), .lpfnWndProc = RawWndProc,
                 .hInstance = GetModuleHandleW(nullptr),
                 .lpszClassName = L"TiltburstRawInput" };
RegisterClassExW(&wc);
HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
RAWINPUTDEVICE rid { .usUsagePage = 0x01,      // Generic Desktop
                     .usUsage     = 0x06,      // Keyboard
                     .dwFlags     = RIDEV_INPUTSINK,   // receive w/o focus
                     .hwndTarget  = hwnd };
if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) return false; // -> fallback
MSG msg;
while (GetMessageW(&msg, nullptr, 0, 0) > 0) {           // WM_QUIT exits
    TranslateMessage(&msg); DispatchMessageW(&msg);
}
```

`WM_INPUT` handler sketch:

```cpp
case WM_INPUT: {
    RAWINPUT ri; UINT size = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &ri, &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1) break;
    if (ri.header.dwType != RIM_TYPEKEYBOARD) break;
    const RAWKEYBOARD& kb = ri.data.keyboard;
    if (kb.MakeCode == 0) break;                    // synthetic, ignore
    bool release = (kb.Flags & RI_KEY_BREAK) != 0;
    uint32_t sc  = kb.MakeCode;                     // scan code set 1
    if (kb.Flags & RI_KEY_E0) sc |= 0xE000;         // extended keys
    // NOTE: Left Shift = 0x2A, Right Shift = 0x36 — distinguished by
    // MakeCode, NOT by E0. Right Ctrl/Alt/arrows carry E0.
    SDL_Scancode sdl_sc = map_set1_to_sdl(sc);      // static table, tb_platform
    // Raw input repeats 'make' while held: edge-filter with a local bitset.
    if (!state_changed(sdl_sc, !release)) break;
    for (action : keymap.actions_for(sdl_sc))
        push_edge({ tb::now_ns(), action, !release, /*source=*/1 });
    break;
}
```

Stop: `PostMessageW(hwnd, WM_CLOSE, 0, 0)`; `WM_CLOSE` handler calls
`PostQuitMessage(0)`; join the thread; `DestroyWindow` + `UnregisterClass`
on the raw-input thread before it exits. `RIDEV_INPUTSINK` delivers input
even when unfocused — required so alt-tab never kills flippers on a
cabinet — but gameplay edges are gated by focus (§9.9).

### 9.6 EvdevInputSource (Linux, raw-input thread)

Device discovery at `start()` and on inotify events for `/dev/input`:

```
for each /dev/input/event* (glob, sorted):
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)
    if fd < 0:
        if errno == EACCES: saw_permission_denied = true
        continue
    # capability check: must be a keyboard-like device
    ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits)
    if !test_bit(EV_KEY, evbits): close; continue
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits)
    if !(test_bit(KEY_LEFTSHIFT, keybits) || test_bit(KEY_ENTER, keybits)):
        close; continue                       # skips mice, power buttons
    clk = CLOCK_MONOTONIC; ioctl(fd, EVIOCSCLOCKID, &clk)
    add fd to poll set                        # NEVER EVIOCGRAB — OS keeps the kbd
```

Read loop: `poll()` on all device fds + the inotify fd, timeout 500 ms
(to observe stop flag). Per `input_event`:

```
if ev.type == EV_KEY and ev.value != 2:        # 0=release 1=press 2=autorepeat
    ts_ns = ev_time_to_monotonic_ns(ev) + g_evdev_offset_ns   # §3
    sdl_sc = map_keycode_to_sdl(ev.code)       # KEY_* -> SDL_Scancode table
    for action in keymap.actions_for(sdl_sc):
        push_edge({ ts_ns, action, ev.value == 1, /*source=*/2 })
```

If discovery ends with **zero opened devices** and
`saw_permission_denied`, print exactly this to stderr and log it at
`warn`, then report unavailable (→ SDL fallback):

```
WARNING: raw keyboard input unavailable: permission denied opening /dev/input/event* devices.
Tiltburst falls back to SDL input (adds a few ms of latency).
To enable low-latency input, add your user to the 'input' group and log out and back in:
    sudo usermod -aG input $USER
```

Device unplug: `read` returns `ENODEV` → close fd, remove from poll set;
if the set becomes empty, deactivate (→ fallback, §9.8) and log `warn`.
Replug is caught by inotify and reactivates the source.

### 9.7 macOS

`SDLInputSource` only, per canon §5.4. The `InputSource` seam exists so an
IOKit HID source can be added post-v1 without touching the sim. Do not
build it in v1.

### 9.8 Runtime selection and fallback (binding)

| Platform | Primary gameplay source | Fallback |
|---|---|---|
| Windows | WinRawInputSource | SDLInputSource |
| Linux | EvdevInputSource | SDLInputSource |
| macOS | SDLInputSource | — |

- SDLInputSource always runs (menus need it). While a raw source is
  `active()`, the SDL source **suppresses gameplay-class edges** (actions
  0–8) and forwards only UI-class ones; otherwise every key would arrive
  twice. UI actions always come from SDL.
- Primary `start()` fails at boot → log `warn` (with the §9.6 text on
  Linux permission failure), SDL source handles gameplay.
- Primary dies at runtime (device loss) → SDL gameplay suppression lifts
  immediately (the `active()` flag is atomic); sim reconciliation (§9.2
  rule 4) emits synthetic edges for any level mismatch at the handover.
- The F3 overlay shows the active source name.

### 9.9 Focus gating (binding)

Main thread maintains `std::atomic<bool> g_app_focused` from SDL focus
events. While unfocused: producers keep tracking the physical bitset but
tag edges; `latch_input` discards gameplay edges and holds the previous
`InputState` with all buttons released. On focus regain, reconciliation
(§9.2 rule 4) syncs to the real held state via synthetic edges. This
prevents typing in another window from flipping flippers (RIDEV_INPUTSINK
and evdev both see global input).

## 10. RNG — PCG32 (the only RNG in the sim)

Canon §5.3: all sim randomness flows through a sim-owned seeded PCG32.
This is the complete implementation; no other RNG (`rand`, `std::mt19937`,
`SDL_rand`, `math.random`) may appear anywhere in `tb_sim` or in Lua
(10-scripting.md replaces `math.random` with `tb.rng`).

```cpp
namespace tb {

class Pcg32 {
public:
    void seed(uint64_t initstate, uint64_t initseq) {
        state_ = 0u;
        inc_   = (initseq << 1u) | 1u;     // stream id; must be odd
        next_u32();
        state_ += initstate;
        next_u32();
    }

    uint32_t next_u32() {                  // PCG-XSH-RR, O'Neill reference
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot        = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, 1). 24-bit mantissa, exact, never returns 1.0.
    float next_float() { return (next_u32() >> 8) * 0x1.0p-24f; }

    // Uniform in [0, bound), bound >= 1. Unbiased (threshold rejection).
    uint32_t next_below(uint32_t bound) {
        uint32_t threshold = (uint32_t)(-bound) % bound;   // = 2^32 mod bound
        for (;;) {
            uint32_t r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }

    // Uniform integer in [lo, hi] inclusive, lo <= hi.
    int32_t range_i32(int32_t lo, int32_t hi) {
        return lo + (int32_t)next_below((uint32_t)(hi - lo) + 1u);
    }

    // Uniform float in [lo, hi).
    float range_f32(float lo, float hi) {
        return lo + (hi - lo) * next_float();
    }

private:
    uint64_t state_ = 0x853c49e6748fea9bULL;   // reference defaults
    uint64_t inc_   = 0xda3e39cb94b95bdbULL;
};

}  // namespace tb
```

### 10.1 Streams and seeding (binding)

- The sim owns exactly two instances, both seeded at `game_start`:
  - `rng_sim`: `seed(game_seed, 0x0000000000000001)` — physics-side
    randomness (e.g. pop-bumper micro-jitter, 08-physics.md).
  - `rng_script`: `seed(game_seed, 0x0000000000000002)` — backs `tb.rng`
    (10-scripting.md). Separate stream so script draws never perturb
    physics sequences.
- `game_seed` comes from `--seed`, a replay header (§13), or (default) one
  `std::random_device` draw on the main thread per new game, passed into
  the sim with the `game_start` command and written to any record file.
- Determinism (canon §5.3): same binary + same seed + same input stream ⇒
  identical simulation. The determinism suite in 16-testing-ci.md compares
  `state_hash()` sequences (§2.4.1, §2.4.3) — mechanical state plus the
  event-sequence component that covers emission order (§6.3) — and the
  final `SimSnapshot` bytes.
- Unit tests must pin the reference sequence: after `seed(42u, 54u)` the
  first three `next_u32()` results are `0xa15c02b7, 0x7b47f409, 0xba1d3330`.

## 11. Configuration — `settings.json`

### 11.1 Schema

Location: `<pref>/settings.json` (§1 step 4). JSON with `//` comments
allowed (parsed with `ignore_comments = true`, canon §5.2). Unknown keys
are preserved on save; missing keys take the defaults shown. Out-of-range
values are clamped to range and logged at `warn`.

This is the **single authoritative key list** for `settings.json`; the M18
settings menu is generated from it (11-game-framework.md §8.4), so a key
that is not here does not exist.

```jsonc
{
  "version": 1,                    // migration gate; bump with a converter
  "video": {
    "present_mode": "auto",        // "auto" | "mailbox" | "vsync"
                                   // auto = MAILBOX if supported else VSYNC
                                   // (applied per window in 07-displays.md §7)
    "brightness": 1.0              // 0.5 .. 1.5, final-blit multiplier (06)
  },
  "render": {                      // consumed by 06-rendering.md / 13-art-direction.md
    "bloom_enabled": true,         // R3.1 toggle: false skips the bloom chain (06 §12)
    "bloom_threshold": 1.0,        // bright-pass THRESH (06 §12.1)
    "bloom_knee": 0.5,             // bright-pass KNEE (06 §12.1)
    "bloom_strength": 0.6,         // composite mix factor (06 §12)
    "crt": false                   // optional CRT scanline/vignette branch of
                                   // the final composite: formulas in
                                   // 13-art-direction.md §10, applied in
                                   // 06 §12.5, which is the last subsection
                                   // of 06 §12. Off by default.
  },
  "audio": {                       // volumes: integers 0 .. 100,
                                   // gain = (v/100)^2 (12-audio.md §3.1)
    "master": 80,
    "sfx":    100,
    "music":  60,
    "ui":     80,
    "period_frames": 0             // 0 = auto-probe the 128→256→512 ladder;
                                   // else the persisted working period (12 §2.1)
  },
  "input": {                       // action -> list of SDL scancode names
                                   // (SDL_GetScancodeFromName); actions §9.1
    "left_flipper":    ["Left Shift"],
    "right_flipper":   ["Right Shift"],
    "left_flipper_2":  ["Left Shift"],
    "right_flipper_2": ["Right Shift"],
    "plunger":         ["Space"],
    "nudge_left":      ["Z"],
    "nudge_right":     ["/"],
    "nudge_up":        ["X"],
    "start":           ["1", "Return"],
    "pause":           ["Escape"],
    "plunger_max_pull_s": 1.5,     // 0.5 .. 3.0 s of hold for 100% power
                                   // (01 §7; charge curve in 08-physics.md)
    "nudge_level": 2               // 1 | 2 | 3 -> Δv table in 08-physics.md
                                   // §7.1; recorded in the replay header (§13)
  },
  "game": {
    "balls_per_game": 3,           // 3 or 5 only; anything else -> 3 + warn
    "tilt_warnings": 2,            // 1 .. 3 free warnings before tilt (01 §7);
                                   // domain {1,2,3}, accounting in
                                   // 11-game-framework.md §5
    "ball_save_seconds": 8,        // 0 .. 15; 0 disables (01 §7, 11 §4.3)
    "replay_award": "extra_ball",  // "extra_ball" | "off" (11 §3.3)
    "replay_score": {}             // per-slug service-menu overrides, e.g.
                                   // { "neon-drift": 5000000 } (11 §3.3)
  },
  "accessibility": {
    "reduce_flashing": false,      // caps strobe/flash rates + intensity
                                   // (11 §10, 13-art-direction.md §12)
    "ball_outline": false,         // high-contrast outline on every ball (11 §10)
                                   // no colorblind_palette key in v1: the
                                   // palette variant is post-v1 (01 §7)
    "screen_shake": true           // false disables cosmetic camera shake
                                   // (01 §7; fx in 06/13)
  },
  "last_table": "neon-drift"       // slug; empty/unknown -> neon-drift
}
```

An unknown scancode name in `input` is skipped with a `warn`; an action
whose list becomes empty falls back to its default binding. Multiple names
per action are allowed (cabinet + desktop keys side by side).
`plunger_max_pull_s` and `nudge_level` are the two non-binding keys in
`input`.

Saves happen on any settings change (debounced 1 s) and at shutdown (§1).

### 11.2 Crash-safe writes (binding, applies to every config file)

Used for `settings.json`, `displays.json` (07-displays.md §5), and high
scores (11-game-framework.md):

1. Serialize to memory.
2. Write to `<file>.tmp` in the same directory.
3. Flush and fsync the file (`FlushFileBuffers` on Windows, `fsync` on
   POSIX), then close it.
4. Atomically replace: POSIX `rename(tmp, file)`; Windows
   `MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
   (`std::filesystem::rename` provides exactly these semantics — use it.)
5. POSIX only: open the directory and `fsync` it so the rename itself is
   durable.

On any step failing: log `error`, delete the tmp file, keep the old file.
Readers therefore always see either the complete old or complete new
content, never a torn file.

## 12. Logging

A tiny thread-safe logger over `fmt`. No third-party logging library.

- **Levels**: `trace(0) < debug(1) < info(2) < warn(3) < error(4)`.
  Runtime threshold: `info` by default, `debug` with `--dev`. Compile-time
  floor `TB_LOG_MIN_LEVEL` (default `trace` in Debug, `debug` in Release)
  compiles lower calls out entirely.
- **Entry**: fixed 256 bytes: `{uint64 ts_ns; uint8 level; uint8 thread;
  char msg[246];}` where `thread` is an enum (main/sim/audio/rawinput/
  other). Messages longer than 245 chars are truncated (fmt::format_to_n),
  never allocated.
- **Hot-path rule (binding)**: `TB_LOG_*` formats into a stack buffer with
  `fmt::format_to_n`, then copies into a global ring of 4096 entries under
  a spinlock held only for the memcpy (~100 ns). **No heap allocation, no
  file I/O, no syscalls** on the calling thread. Sim and audio threads may
  therefore log freely at `warn`+; `trace` in per-tick inner loops must be
  compiled out in Release.
- **File sink**: the main thread drains new ring entries once per frame
  and writes them to the log file (buffered `fwrite`, `fflush` on `warn`+
  and at shutdown). Crash handlers (§1 step 2) dump the whole ring.
- **Format**: `[+123.456789] [WARN ] [sim  ] message\n` — seconds since
  init with 6 decimals, fixed-width level and thread tags.
- `log_warn_ratelimited(...)`: at most 1 emission per second per call
  site (static counter), used in the sim loop (§6).
- The last 64 `warn`+ entries are viewable in the F3 overlay page.

## 13. Record / replay files

Binary, little-endian, extension **`.tbreplay`**. Written by `--record`,
read by `--replay`, produced/consumed also by `tb_autoplay` (workflow in
14-authoring-guide.md). This runtime edge-stream format is owned here; the
hand-authorable JSON test tapes are a separate format owned by
16-testing-ci.md §2.4.2 and expand into exactly this stream (§13.1).

```
header (96 bytes):
  char     magic[4]     = "TBRP"
  uint16   version      = 1
  uint16   _pad
  uint64   seed         # game_seed (§10.1)
  char     table[64]    # slug, NUL-padded
  uint64   tick_count   # ticks recorded (patched on close)
  uint8    nudge_level  # input.nudge_level in force (§11.1, 08-physics.md §7.1)
  uint8    _reserved[7]
records (repeated):
  uint64   tick         # tick that consumed the edge (§9.2 rule 5)
  uint16   action
  uint8    pressed
  uint8    source       # as produced, incl. synthetic; informational
  uint32   _pad
```

Recording captures edges **after** latch/reconciliation, so replaying
injects the exact per-tick edge sequence the sim consumed (source = 4) —
including sub-millisecond taps, which the §13.1 tape format cannot express.
Replay asserts the header table slug matches the loaded table and errors
out (exit 1) on mismatch, and applies the header's `nudge_level` for the
run (nudge strength is sim input — 08-physics.md §7.1). Determinism
guarantee and CI usage: 16-testing-ci.md.

### 13.1 JSON test tapes (16-testing-ci.md §2.4.2)

`--replay` (and `tb_autoplay --replay`) also accept the JSON tape format
`<slug>.replay.json` — `[tick, buttons_bitmask]` entries, each mask holding
from its tick (inclusive) until the next entry, owned by 16-testing-ci.md
§2.4.2 — selected by file extension. A tape is expanded into the §13 edge
stream before injection: at each entry's tick, every **action** whose level
differs from the previous entry's becomes one edge (ascending action index,
`source = 4`); the mask preceding the first entry is 0, so a tape that opens
with a non-zero mask presses those actions at its first tick. The tape's
`seed` becomes `game_seed` (§10.1) and its `table_slug` is checked against
the loaded table exactly as the §13 header is — mismatch errors out (exit
1). The bit → action mapping below fulfils 16 §2.4.2's delegation
("05-engine-core.md maps its logical input actions onto these bits"):

| Tape bit (16 §2.4.2) | Action index (§9.1) |
|---|---|
| 0 `flipper_left` | 0 `left_flipper` |
| 1 `flipper_right` | 1 `right_flipper` |
| 2 `flipper_upper_left` | 2 `left_flipper_2` |
| 3 `flipper_upper_right` | 3 `right_flipper_2` |
| 4 `plunger_pull` | 4 `plunger` |
| 5 `launch` | 4 `plunger` — alias; v1 has one plunger/launch action, which button-launch and auto plungers also read (08 §6, 09 §4). Two bits, one action: the level of action 4 is `bit4 OR bit5`, and an edge is emitted only when that OR-ed level changes — so bits 4 and 5 changing together yield one edge, and a change that leaves the OR unchanged (e.g. bit 4 rising while bit 5 is already set, or the two swapping) yields none. |
| 6 `nudge_left` | 5 `nudge_left` |
| 7 `nudge_right` | 6 `nudge_right` |
| 8 `nudge_forward` | 7 `nudge_up` |
| 9 `start` | 8 `start` |

Bits 10–15 must be 0 (reserved, 16 §2.4.2); a tape that sets any of them is
rejected before the first tick with a one-line error (exit 1), never
silently ignored. Tapes carry no `nudge_level`; tape replays run at the
`input.nudge_level` default, 2 (§11.1).

## 14. Latency instrumentation

Instrumented from M1 onward (R2 is a hard requirement; regressions must be
visible the day they happen).

### 14.1 Per-stage timestamp ring

`LatencyRecord`, ring of 512, written lock-free (sim writes stages 1–3,
main writes 4–5 into the record matching the snapshot tick it rendered):

| Stage | Field | Stamped by | When |
|---|---|---|---|
| 1 | `input_ts_ns` | input source | edge produced (§9) |
| 2 | `latch_ts_ns` | sim | edge consumed by `latch_input` |
| 3 | `publish_ts_ns` | sim | snapshot containing the tick published |
| 4 | `render_begin_ns` | main | frame using that snapshot starts recording |
| 5 | `present_ns` | main | just before `SDL_SubmitGPUCommandBuffer` of that frame |

A record is keyed by `(tick, newest input_ts consumed that tick)`; ticks
with no input still produce records with stage 1 empty (they feed frame
stats but not input-age stats). Derived metrics: `input→latch` (the R2/R2.1
gate: **p99.9 < 4 ms over ≥ 10,000 scripted press edges** — R2 covers this
hop plus the tick itself), `latch→present` (software motion-to-photon minus
scanout).

**Cumulative histogram for the R2.1 gate (binding).** p99.9 over ≥ 10,000
edges cannot be read off a 512-entry ring, so the sim additionally keeps a
lifetime `input→latch` histogram, written on the same code path as the
ring: **129 `uint32` bins — 128 linear bins of 62.5 µs spanning 0–8.000 ms,
plus one overflow bin (≥ 8 ms)** — with a `uint64` total-edge counter, reset
only at process start and counting **press edges only** (releases are not
part of the gate). A percentile is the value of the bin where the running
sum first reaches `ceil(q · n)`; reported as that bin's upper edge, so the
result is conservative (never optimistic) at 62.5 µs granularity. With
exactly n = 10,000 press edges, p99.9 is the 9,990th sample in ascending
order — the 11th largest — so **at most 10 edges may exceed 4 ms** and the
run needs ≥ 10,000 edges before the number is quotable at all. The F3
overlay (§14.3) shows `n` next to the p99.9 value, and `--latency-test`
(§14.4) writes the whole histogram as a trailing CSV comment block so the
M4/M20 evidence is a file, not a screenshot reading. The 512-record ring
remains the source of the live p50/p95/max display only.

### 14.2 F1 — perf overlay

Rendered by 06-rendering.md's debug text path; data owned here:

- fps (1 s window) and frame-time graph: last 240 frames, bar per frame,
  y-scale 0–33.3 ms, horizontal line at the display's refresh period.
- tick time µs (p50/p99 over last 1000 ticks), sim jitter p99 (§6.2).
- input-to-tick age: `latch_ts − input_ts` of the newest input edge, ms.
- dropped frames: count of frames with frame time > 1.5 × refresh period.
- `overruns_total`, `dropped_ticks_total` (§6.1).
- ring drop counters: render/game/input rings (§8.3, §9.2) plus the sound
  ring's `dropped_events` from the audio stats (§8.2, 12 §4.1).

### 14.3 Overlay keys

| Key | Toggles |
|---|---|
| F1 | perf overlay (§14.2) |
| F2 | debug draw: collider wireframes, ball velocity vectors (06-rendering.md) |
| F3 | latency detail: per-stage table p50/p95/max (ms, 2 decimals) over the last 512 records, **plus cumulative `input→latch` p99.9 shown as `p99.9=X.XX ms (n=NNNNN)`** from the §14.1 histogram — percentile and sample count always together, active input source name, `backglass_skips` (07 §8), recent warnings page |
| F4 | event log: scrolling feed of the most recent 64 sim/script events seen on main (render + game rings, §8.2), newest at the bottom — 01 §6 |

Overlay state is main-thread-only and never affects the sim (determinism).

### 14.4 `--latency-test` mode

External photodiode measurement of true motion-to-photon:

- Loads no table. Black playfield. On every `left_flipper`/`right_flipper`
  **press** edge, a 256×256 px pure-white square is drawn at the corner of
  the playfield viewport nearest (0,0) table space for 100 ms.
- Each flash appends a CSV row to `logs/latency-YYYYMMDD-HHMMSS.csv`:
  `input_ts_ns,latch_ts_ns,present_ns` for the first frame that drew the
  square. The user's photodiode + scope measures press-to-light; the CSV
  gives the software share, so scanout/display latency = measured − CSV.
- On exit the same file gets a trailing comment block with the §14.1
  histogram: one `# bin_us_upper,count` line per non-empty bin, then
  `# n=<press edges>, p50, p99, p99.9` in ms. This file is the R2.1
  evidence artifact (p99.9 < 4 ms over ≥ 10,000 scripted press edges);
  a run with n < 10,000 prints `# n=<…> INSUFFICIENT` and the gate is
  not considered measured.
- Present mode and input source follow normal settings, so the mode
  measures the shipped configuration. The F3 overlay works in this mode.

## Common pitfalls

- **Sleeping the whole tick interval.** `sleep_until(target)` alone
  oversleeps by up to 1 ms on Windows. Follow §6: sleep to target − 300 µs,
  then spin. Conversely, do not spin the full millisecond (burns a core).
- **Queueing framework events for the next tick.** Phase 3 dispatches
  `ball_end`, `tilt`, `multiball_start`, … to Lua *synchronously while the
  FSM emits them*, inside the tick that produced them (§6.3). Deferring
  them by a tick breaks `tb.add_bonus` inside `ball_end` (11 §4.5) and
  changes the event order, which moves the event-sequence component of
  `state_hash()` (16 §2.4.1), so the determinism gate fails.
- **Running the GameFsm before script dispatch, or interleaving them.**
  The order is physics → sim events → FSM (with its own synchronous
  dispatch) → timers/GC/publish, exactly as in 10 §2.2 and 11 §1. Stepping
  the FSM inside phase 2 makes handler order depend on which sim event ran
  first.
- **Reading input at frame rate.** Flipper edges must be latched by the
  sim thread immediately before each 1 ms tick (§6, §9.2), not gathered in
  the render loop. The SDL event pump is a *producer*, not the latch point.
- **Losing sub-tick taps.** Deriving button state only from the atomic
  bitset drops press+release pairs that fit inside 1 ms. The edge rings
  exist precisely for this; implement §9.2 rule 3 and its reconciliation.
- **Making the snapshot reader wait.** `TripleBuffer::read()` is one
  atomic exchange, no loop, no lock, and returns stale data when the sim
  hasn't published — that is correct behavior. Never block the main thread
  on "fresh" data.
- **Relaxed atomics on the triple buffer or rings.** The `acq_rel`/
  `release`/`acquire` orders in §7.2/§8.3 are load-bearing; on ARM (Apple
  Silicon) `relaxed` produces real torn reads that x86 testing won't show.
- **Fanning sounds out as `SimEvent`s.** The sim→audio ring carries
  16-byte `SoundEvent`s only, capacity 1024 (§8.2; 12-audio.md §4.1 owns
  it). Patch, velocity and pan are resolved on the *sim* thread; looking up
  the table's sound map inside the miniaudio callback breaks the callback
  contract (12 §2.3) and determinism alike. Its overflow drops the newest
  event, not the oldest.
- **Producer-side drop-oldest by moving the read index.** The producer
  never touches `read_`; drop-oldest is implemented as overwrite +
  consumer lap detection (§8.3). Anything else races.
- **Calling wall-clock, `random_device`, or `SDL_GetTicks*` inside
  `sim.step()` or Lua.** Breaks determinism (canon §5.3). Time enters the
  sim only as tick counts; entropy only as the game seed.
- **Two GPU threads.** Backglass rendering stays on main with a
  non-blocking acquire (canon §5.4). Do not spawn a render thread even
  though ARCHITECTURE.md ADR-004 sketches one — canon wins (PLAN §5.10).
- **Using SDL key events for gameplay while a raw source is active.**
  Causes double edges. Implement the suppression rule in §9.8.
- **Forgetting `RI_KEY_E0` / autorepeat filtering** on Windows raw input:
  Right Ctrl becomes Left Ctrl, and held keys spam press edges. §9.5.
- **`EVIOCGRAB` on evdev.** Grabbing steals the keyboard from the OS; the
  user cannot alt-tab or type. Never grab (§9.6).
- **fopen/printf on the sim or audio thread.** All file I/O for logging
  happens on main (§12). The logger's hot path is format-to-stack + memcpy.
- **Non-atomic settings writes.** A crash mid-write must never corrupt
  `settings.json`; follow §11.2 exactly (tmp + fsync + rename).
- **Exiting 1 (or 0) on a bad flag.** CI reads exit 2 as "skip" and exit 1
  as "this job failed" (§2.1, 16 §5): a typo in a workflow flag that exits 1
  turns the matrix red for the wrong reason, and one that exits 0 hides a
  step that never ran. Unknown flag, bad value, contradictory combination →
  usage to stderr, exit 2.
- **Quoting a latency percentile without its sample count.** The gate is
  p99.9 < 4 ms over ≥ 10,000 scripted press edges (§14.1); a p99.9 taken
  from the 512-entry ring is a p99.9 of 512 samples and means nothing.
  Report `p99.9` and `n` in the same line, always.

## Done when

- [ ] `tiltburst` boots through §1 on all three OSes; `--headless` runs
      with no video/GPU/audio initialization (verified by CI on a machine
      with no display server).
- [ ] All CLI flags in §2 parse and work; `tiltburst --version` prints
      `tiltburst <version>` to stdout and exits **0** (04 M0 acceptance, and
      the launch probe of `perf_startup.cold_boot_to_attract`, 16 §2.10);
      every unrecognized flag, malformed value, and invalid combination
      prints usage to stderr and exits **2** per the §2.1 exit-code contract
      (04 M0 and the CI renderer-smoke step read 2 as *skip*), while §1's ✗
      failures exit 1.
- [ ] The GPU device is created from `available_shader_formats()` (§1
      step 9, 06 §16.4) — verified by a run on a build with DXIL absent,
      which still boots on Vulkan — with `debug_mode = cli.dev ||
      TB_DEV_BUILD`, the same expression at both call sites (§1 step 9 and
      06 §3): a shipping Release run without `--dev` creates the device with
      the debug layer off, and the same binary with `--dev` creates it on.
- [ ] Sim thread holds 1000 Hz: 60 s idle run on reference hardware shows
      p99 jitter ≤ 100 µs, max ≤ 500 µs, zero overruns (§6.2 acceptance).
- [ ] Artificially loading a tick (test hook: 200 ms sleep in `sim.step`)
      triggers the §6.1 clamp: exactly 50 catch-up ticks, warn logged,
      counters visible in F1.
- [ ] Tick phase order (§6.3) is observable and correct: a probe script
      recording `(event, tick, sequence)` shows a sim event (phase 2), the
      framework event the FSM emits from it (phase 3) and a 0 ms timer
      (phase 4) on the **same** tick in that order; `tb.add_bonus` called
      inside a `ball_end` handler lands in that ball's counted bonus
      (11 §4.5).
- [ ] TripleBuffer passes a 2-thread stress test (writer 1 kHz, reader
      spinning): reader always observes a fully-consistent snapshot whose
      tick is monotonic non-decreasing; TSan clean.
- [ ] EventRing passes SPSC stress with a deliberately slow consumer:
      `delivered + dropped == pushed`, delivered events are in order, no
      torn events (checksum field test); TSan clean. Same stress on the
      1024-slot `SoundEvent` ring: `delivered + dropped == pushed` with the
      **newest** events dropped on overflow (§8.2, 12 §4.1).
- [ ] A 0.5 ms scripted press+release (test input source) reaches the sim
      as two edges in one tick, in order (§9.2 rule 3 test).
- [ ] Windows: WinRawInputSource delivers edges with the app unfocused
      (and sim ignores them per §9.9); Left/Right Shift distinguished.
- [ ] Linux: EvdevInputSource works with `input`-group membership; without
      it, the exact §9.6 warning text prints once and SDL fallback plays.
- [ ] PCG32 unit test pins the reference sequence (§10.1) and
      `next_below` passes a chi-squared uniformity test for bound = 6 over
      1e6 draws (p > 0.001).
- [ ] Record 60 s of play, replay with same binary + seed: final snapshot
      bytes identical and every `state_hash()` identical — including its
      event-sequence component, which is what makes emission order (§6.3)
      part of the gate (determinism gate, 16-testing-ci.md §2.4.1/§2.4.3).
- [ ] `settings.json` survives kill -9 during a save storm (loop of 1000
      saves with random kills): file always parses as either old or new
      content, never truncated.
- [ ] F1–F4 overlays toggle and show every §14.2/§14.3 datum;
      `--latency-test` flashes the square and writes the CSV; measured
      input→latch **p99.9 < 4 ms over ≥ 10,000 scripted press edges** on
      reference hardware, read from the §14.1 cumulative histogram (the
      single binding statement, identical in 01 R2.1/§9 and 04 M4).
