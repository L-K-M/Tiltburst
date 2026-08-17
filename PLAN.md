# Tiltburst — Master Plan

**Status:** Ready for implementation
**Audience:** the implementor LLM that builds Tiltburst end to end without human
intervention, and any human contributor.

Tiltburst is a cross-platform (Windows, Linux, macOS) digital pinball game for
converted real cabinets and ordinary desktops. It ships with five original
tables, local multiplayer for 1–4 players, a neon retro (90s arcade × 60s
atomic-age) visual style with particle effects, and a fully text-based table
format designed so that LLMs as well as humans can create new, genuinely
different tables — layout, rules, art, and sound — without any binary assets.

The technical baseline is [ARCHITECTURE.md](ARCHITECTURE.md), as amended by
[docs/plan/02-decisions.md](docs/plan/02-decisions.md). This file is the index
and the **canon**: the set of facts every document, every PR, and every line of
code must agree on.

---

## 1. Reference hardware

The primary target is a 1960s pinball cabinet conversion:

- Windows PC inside the cabinet.
- **Playfield:** 1080p TV mounted vertically (portrait). The OS may report it
  as 1920×1080 landscape (we rotate in the projection) or 1080×1920.
- **Backglass:** a roughly square TV showing the back panel (scores, modes,
  attract art).
- Input: keyboard-encoded cabinet buttons (flippers, plunger, start, nudge).

Both displays are 60 Hz, so the hard requirement is: **the playfield never
drops below 60 fps and input latency is as low as the platform allows**
(see §3 targets). Higher refresh rates must be supported on other hardware.
Secondary target: a normal desktop/laptop with one display (windowed dev mode).

## 2. How to use this plan (implementor protocol)

1. Read this file completely. Then read
   [03-process.md](docs/plan/03-process.md) and
   [04-milestones.md](docs/plan/04-milestones.md) completely.
2. Work strictly milestone by milestone (M0 → M20). One milestone = one PR
   (a milestone may be split into consecutive PRs `M13a`, `M13b` if it would
   exceed ~3,000 added lines).
3. For every PR, follow the review loop in 03-process.md: open PR → wait for
   the automated **GLM 5.3 Code Review** (workflow
   `.github/workflows/zai-code-review.yml`) → address or rebut every comment →
   push → repeat until steady state (no new actionable feedback) → merge
   squash → start the next milestone. Never start milestone N+1 before
   milestone N is merged.
4. Before implementing a subsystem, read its spec document (see §4 map).
   Specs are binding. If a spec is wrong or incomplete, fix the spec in the
   same PR and record the deviation as a new ADR in 02-decisions.md and a note
   in `docs/JOURNAL.md`.
5. Never ask a human. Every foreseeable blocking situation has a documented
   fallback (03-process.md §Fallbacks). If something is genuinely undecidable,
   choose the option that best serves the §8 Definition of Done, document it,
   and continue.

## 3. Product requirements (binding)

Full detail in [01-product.md](docs/plan/01-product.md). Requirement IDs are
used in milestone acceptance criteria.

| ID | Requirement |
|----|-------------|
| R1 | Playfield ≥ 60 fps at all times on reference hardware; supports native refresh up to 240 Hz |
| R2 | Lowest achievable input latency: OS-delivered key edge → sim response < 4 ms (the measurement boundary — encoder debounce and USB polling sit outside it; 01-product.md R2.1, end-to-end photodiode path in 05-engine-core.md §14.4); end-to-end motion-to-photon < 25 ms at 120 Hz-class hardware, hardware-limited at 60 Hz |
| R3 | Modern neon retro look: particle effects, glow/bloom, 90s arcade + 60s atomic-age styling |
| R4 | Automatic display detection: portrait display → playfield, squarest display → backglass; manual override; single-display fallback |
| R5 | Ships with 5 distinct complete tables |
| R6 | Local multiplayer: 1–4 players alternating (classic), plus a 2-player Duel mode |
| R7 | Physics feel comparable to real pinball; supports multiple flippers per table, magnets, ramps/layers, kickers, drop banks, spinners, captive balls, multiball |
| R8 | Runs on Windows, Linux, macOS from one codebase |
| R9 | Tables are fully authorable as text (layout JSON, Lua rules, vector art, synthesized audio) by LLMs and humans, with validator + autoplay tooling |
| R10 | The entire product is implementable by an LLM working autonomously through reviewed PRs |

## 4. Document map

All plan documents live in `docs/plan/`. Read the listed docs before starting
the milestones that cite them.

| Doc | Owns | Primary milestones |
|-----|------|--------------------|
| [01-product.md](docs/plan/01-product.md) | Vision, requirements, UX flows, pinball glossary | all |
| [02-decisions.md](docs/plan/02-decisions.md) | ADR-005+, amendments to ARCHITECTURE.md, resolved open questions | all |
| [03-process.md](docs/plan/03-process.md) | Repo conventions, PR/review loop, autonomy protocol, fallbacks | all |
| [04-milestones.md](docs/plan/04-milestones.md) | M0–M20 scope, files, tests, acceptance criteria | all |
| [05-engine-core.md](docs/plan/05-engine-core.md) | Main loop, timing, input (raw paths), config, logging, RNG | M1, M4 |
| [06-rendering.md](docs/plan/06-rendering.md) | SDL3 GPU renderer, SDF primitives, particles, bloom, text | M3, M13 |
| [07-displays.md](docs/plan/07-displays.md) | Display detection, windows, per-display pacing, rotation | M12 |
| [08-physics.md](docs/plan/08-physics.md) | Solver, CCD math, flippers, every element's physics | M2, M4, M6–M8 |
| [09-table-format.md](docs/plan/09-table-format.md) | table.json schema, element params, prefab generators, validation rules | M5–M8, M15 |
| [10-scripting.md](docs/plan/10-scripting.md) | Lua sandbox, full `tb.*` API, event payloads, patterns | M9 |
| [11-game-framework.md](docs/plan/11-game-framework.md) | Game state machine, players/multiplayer, tilt, scores, menus | M10, M18 |
| [12-audio.md](docs/plan/12-audio.md) | miniaudio mixer, sfxr-style synth, tracker music format | M11, M14 |
| [13-art-direction.md](docs/plan/13-art-direction.md) | Style guide, palettes, vector-art format (TBArt), decal prefabs | M13 |
| [14-authoring-guide.md](docs/plan/14-authoring-guide.md) | How LLMs/humans design fun tables; tooling workflow; metrics | M15–M17 |
| [15-launch-tables.md](docs/plan/15-launch-tables.md) | Full designs of the 5 shipped tables | M5, M9, M16, M17 |
| [16-testing-ci.md](docs/plan/16-testing-ci.md) | Test strategy, determinism, perf gates, CI workflows | M0, all |

## 5. Canon

Facts below are the single source of truth. A document or PR that contradicts
them is wrong (or must amend them explicitly via an ADR in 02-decisions.md).

### 5.1 Naming and repository layout

- Product/app name **Tiltburst**; binary `tiltburst`; C++ namespace `tb`.
- Tables are "tables" in code and docs ("boards" is accepted user vocabulary).

```
/src
  /core        # timing, logging, config, RNG (PCG32), math helpers
  /platform    # SDL3 bootstrap, raw HID input (Win Raw Input, Linux evdev), file paths
  /sim         # physics + table logic. HEADLESS: must not include render/platform/audio
  /render      # SDL3 GPU backend, SDF primitives, particles, bloom, text
  /audio       # miniaudio device, mixer, sfxr synth, tracker
  /table       # table pack loading, JSON schema, prefab generators
  /game        # game state machine, players, scoring, menus, backglass UI
  /tools       # tb_validate, tb_autoplay, tb_screenshot (CLI tools)
/shaders       # HLSL source, compiled at build time
/assets        # fonts (OFL, vendored), built-in SFX patches, shared decals
/tables        # shipped table packs: /tables/<slug>/
/tests         # gtest suites; mirrors /src structure
/docs          # this plan, JOURNAL.md, generated notes
```

CMake targets: static libs `tb_core`, `tb_platform`, `tb_sim`, `tb_render`,
`tb_audio`, `tb_table`, `tb_game`; executables `tiltburst`, `tb_validate`,
`tb_autoplay`, `tb_screenshot`, test binary `tb_tests`.
Dependency rule: `tb_sim` depends only on `tb_core`.

### 5.2 Technology stack (pinned)

| Layer | Choice | vcpkg port |
|---|---|---|
| Language / build | C++20, CMake ≥ 3.28, vcpkg manifest mode | — |
| Window/input/events | SDL3 | `sdl3` |
| Rendering | **SDL3 GPU API** (ADR-005; Vulkan deferred, renderer stays behind an interface) | via `sdl3` |
| Shaders | HLSL → SDL_shadercross → SPIR-V/DXIL/MSL at build time | FetchContent |
| Physics | Custom (ARCHITECTURE.md ADR-003), spec in 08-physics.md | — |
| Scripting | Lua 5.4 + sol2, sandboxed, deterministic | `lua`, `sol2` |
| Audio | miniaudio | `miniaudio` |
| JSON | nlohmann-json (comments enabled via `parse(..., ignore_comments=true)`) | `nlohmann-json` |
| Text/format | fmt | `fmt` |
| Fonts/images | stb (truetype, image, image_write) | `stb` |
| Tests | GoogleTest | `gtest` |

### 5.3 Units, coordinates, constants

- SI units: meters, kilograms, seconds, radians internally (`_deg` suffix in
  JSON where degrees are friendlier).
- Playfield coordinates: origin at the **bottom-left corner of the play area
  as the player sees it** (flipper end). +x right, +y up-table (away from the
  player), z = height above the playfield surface (used only on ramps/layers).
- The table slope is simulated as effective gravity in the plane:
  `a = g·sin(slope)` along −y, with `g = 9.81 m/s²`, default `slope = 6.5°`.
- Default play area 0.52 m × 1.04 m (per-table override).
- Ball: radius 0.0135 m, mass 0.08 kg, default 4 physical balls in the trough.
- Max ball speed clamp: 12 m/s.
- Physics tick: **fixed 1000 Hz** (`dt = 0.001` exactly). Sim never reads the
  wall clock; all randomness via the sim-owned seeded PCG32 (`tb.rng` in Lua).
- Determinism: same binary + same seed + same input stream ⇒ identical
  simulation, always. Cross-platform bit-exactness is a non-goal.

### 5.4 Simulation and threading model

Threads (exactly these in v1 — no general job system):

1. **main** — SDL event pump + playfield rendering at native refresh
   (frames-in-flight 1, present mode MAILBOX preferred, else VSYNC), plus
   backglass rendering at a reduced ~30 Hz cadence using a **non-blocking**
   swapchain acquire (skip the backglass frame if its swapchain isn't ready)
   so the backglass can never stall the playfield. All GPU submission stays
   on this thread — SDL3 GPU device access is single-threaded by design in
   Tiltburst.
2. **sim** — 1000 Hz fixed-timestep loop; late-latches freshest input before
   each tick; publishes a triple-buffered `SimSnapshot` (tick, balls, element
   visual states, light states) plus `SimEvent` ring buffers (collisions,
   scores, sounds) consumed by the render/audio/game layers.
3. **audio** — miniaudio device callback; sounds triggered from sim events
   with sample-accurate scheduling, never from render frames.
4. **raw-input** — platform raw input (Windows Raw Input via a hidden
   message-only window; Linux evdev; macOS falls back to SDL events in v1),
   writing an atomic latest-state that sim late-latches. SDL events remain the
   always-available fallback and drive menus.

### 5.5 Table pack format

A table is a directory `tables/<slug>/` of plain text:

| File | Contents | Spec |
|---|---|---|
| `table.json` | Geometry + elements + materials + prefab instances | 09 |
| `rules.lua` | Game rules script (sandboxed Lua) | 10 |
| `art.json` | Layered TBArt vector art (SDF primitives, palettes) | 13 |
| `audio.json` | SFX synth patches + tracker music patterns | 12 |
| `assets/` (optional) | PNG decals / WAV sounds for human authors | 09 |

JSON files may contain `//` comments. Everything an LLM needs to author a
complete table is expressible in these four text files.

### 5.6 Canonical element types (`table.json` `type` values)

`wall`, `post`, `flipper`, `plunger`, `pop_bumper`, `slingshot`,
`standup_target`, `drop_target_bank`, `spinner`, `gate`, `rollover`,
`kicker`, `ramp`, `magnet`, `captive_ball`, `ball_lock`, `outhole`,
`trough`, `light`, `toy`.

Notes: rubbers are a wall/post material (`"material": "rubber"`), not a type.
Ramps are constrained 1-D paths with a height profile; upper playfields are
free 2-D areas on `layer: 1`. Wireforms are ramps with different art.

Prefab generators (macro elements that expand to primitives, spec in 09):
`flipper_pair_standard`, `plunger_lane`, `sling_pair`, `inlane_outlane_pair`,
`orbit`, `ramp_standard`, `inner_loop`, `horseshoe`, `pop_cluster`,
`drop_bank_n`, `top_lanes_n`.

### 5.7 Canonical script API surface

Lua namespace `tb`. Events via `tb.on(name, handler)`; canonical event names:

`game_start`, `ball_start`, `ball_end`, `game_end`, `player_up`,
`ball_launched`, `switch_hit`, `target_down`, `bank_complete`,
`spinner_spin`, `rollover`, `kicker_enter`, `ramp_made`, `drain`,
`ball_save_expired`, `tilt_warning`, `tilt`, `ball_lock`,
`captive_full_travel`, `multiball_start`, `multiball_end`, `timer_tick`.

Core actions (full signatures + payloads in 10-scripting.md):
`tb.score`, `tb.add_bonus`, `tb.set_multiplier`, `tb.award_extra_ball`,
`tb.light_on`, `tb.light_off`, `tb.light_blink`, `tb.play_sound`,
`tb.play_music`, `tb.stop_music`, `tb.kick`, `tb.kick_hold`,
`tb.release_lock`, `tb.magnet_on`, `tb.magnet_off`, `tb.magnet_pulse`,
`tb.set_flipper_enabled`, `tb.timer`, `tb.cancel_timer`, `tb.ball_save`,
`tb.add_ball`, `tb.drop_bank_reset`, `tb.gate_open`, `tb.gate_close`,
`tb.show_message`, `tb.backglass`, `tb.state` (per-player, auto-swapped on
player change), `tb.game` (read-only session info), `tb.table_info`
(read-only table metadata), `tb.rng`, `tb.rng_range`.

Sandbox: no `io`, `os`, `require`, `load`; `math.random` replaced by
`tb.rng`; instruction-count watchdog with a budget shared across all
handlers per tick (02-decisions.md ADR-006, 10-scripting.md §2.4). Scripts
run on the sim thread at tick granularity and must be deterministic.

### 5.8 Shipped tables (R5)

| Slug | Name | Theme | Signature mechanic |
|---|---|---|---|
| `neon-drift` | Neon Drift | Synthwave night-street racing | Magnet "drift corner" + gear-shift drop banks; 3 flippers |
| `atomic-diner` | Atomic Diner | 60s googie space-age diner | Order-completion modes, captive-ball milkshake, upper mini-playfield |
| `tilt-o-tron` | Tilt-O-Tron | Retro-futurist robot factory | Build-a-robot drop banks, magnet crane ball lock, 4-ball multiball |
| `cosmic-carnival` | Cosmic Carnival | Space circus | Cannon skill shot, spinner-heavy juggling multiball |
| `voltage-vandals` | Voltage Vandals | Electric-punk heist | Timed heist modes, alarm magnet grid, risky outlane gates |

Plus `test-lab`: a minimal valid table used only by tests and docs (09, 10).

### 5.9 Displays (R4)

Auto-detection heuristic (detail in 07-displays.md): enumerate displays; a
display with h/w ≥ 1.4 is a playfield candidate (largest wins); among the
rest, aspect closest to 1.0 becomes the backglass. If no display has
h/w ≥ 1.4 (the reference cabinet: a physically rotated TV that still
reports 1920×1080 landscape), the largest landscape display becomes the
playfield, rendered rotated 90° when a near-square (squareness ≥ 0.70)
backglass candidate also exists (07-displays.md §3 is normative). Rotation
always happens via the projection matrix — never OS rotation. Assignment persisted to
`displays.json` in the user config dir (`SDL_GetPrefPath("tiltburst",
"tiltburst")`); explicit config always beats heuristics; single display ⇒
playfield only. Windows are borderless fullscreen.

### 5.10 Precedence

Canon (this §5) > per-domain spec docs (01–16) > ARCHITECTURE.md as amended
by 02-decisions.md > implementor judgment. Conflicts are fixed by PR that
updates the losing document, with a JOURNAL.md note.

## 6. Milestones and PR schedule

One row = one PR (splittable per §2). Full scope, file lists, tests, and
acceptance criteria in [04-milestones.md](docs/plan/04-milestones.md).

| # | Title | Proves |
|---|-------|--------|
| M0 | Repository scaffold & CI | Builds + tests green on 3 OS |
| M1 | App skeleton & fixed-timestep loop | Window, GPU device, 1000 Hz sim loop, timing overlay |
| M2 | Headless simulation core & determinism | Ball + CCD vs segments/arcs, replay determinism test |
| M3 | Renderer v1 & debug draw | Ball + colliders visible, portrait rotation, 60 fps |
| M4 | Flippers, low-latency input & latency overlay | Catch/cradle/backhand feel scenarios pass |
| M5 | Table format v1 & Neon Drift greybox | Load table.json, playable greybox table |
| M6 | Standard elements I | Slings, pops, standups, rollovers, gates, spinner |
| M7 | Standard elements II | Kickers, drop banks, captive ball, trough & ball save |
| M8 | Ramps, layers & magnets | 2.5D ramps, upper layer, magnet feel |
| M9 | Lua scripting & Neon Drift rules v1 | Full rules API, scored game start-to-finish |
| M10 | Game framework: players, tilt, high scores | 1–4 player alternating game, nudge/tilt, persistence |
| M11 | Audio engine & SFX synth | <10 ms audio, sfxr patches, sounds from sim ticks |
| M12 | Multi-display & backglass | Auto-detection, backglass scores, independent pacing |
| M13 | Art system, particles & Neon Drift beauty pass | TBArt, glow/bloom, particles; table looks shipped |
| M14 | Music tracker & attract mode polish | Per-table music, attract loop |
| M15 | Validator, autoplay harness & screenshot tool | tb_validate, tb_autoplay metrics, tb_screenshot |
| M16 | Second table: Atomic Diner | Authoring pipeline dogfood via 14-authoring-guide.md |
| M17 | Tables 3–5 | All five tables complete |
| M18 | Menus, settings, input remap & Duel mode | Full UX, R6 complete |
| M19 | Performance hardening & packaging | Perf gates, installers/archives for 3 OS |
| M20 | Release 1.0 | Definition of Done audit |

## 7. Autonomous development protocol (summary)

Details and exact commands in [03-process.md](docs/plan/03-process.md).

- Branch `milestone/M<NN>-<slug>`; PR title `M<NN>: <title>`; squash merge.
- After each push, the GLM 5.3 review workflow runs automatically (skips
  drafts). Poll for its review; address every comment with a code change or a
  reasoned rebuttal reply. **Steady state** = a review cycle that produces no
  new actionable items. Then merge.
- If the reviewer is unavailable (no review within ~20 min of CI green),
  run the self-review checklist in 03-process.md and merge; note it in
  JOURNAL.md.
- CI (build + tests, 3 OS) must be green before merge. Never merge a known
  correctness bug even if the reviewer is silent about it.
- Keep `docs/JOURNAL.md` (append-only): per milestone — what shipped,
  deviations, new ADRs, open worries.

## 8. Definition of Done, v1.0 (M20 audit checklist)

- [ ] R1–R10 all demonstrably met (01-product.md maps each to evidence)
- [ ] All five tables playable start-to-finish with rules, art, sound, music
- [ ] 1–4 player alternating game + Duel mode work on the cabinet layout
- [ ] Auto display detection works: portrait+square, single display, override
- [ ] Determinism suite green; perf gates green; no known crash bugs
- [ ] `tb_validate` + `tb_autoplay` pass on all shipped tables
- [ ] A new table can be authored end-to-end using only 14-authoring-guide.md
- [ ] Packaged builds boot on all three OSes from a clean machine
