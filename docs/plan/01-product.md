# 01 — Product Specification

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: none (root product document). Cross-references: 02-decisions.md,
05-engine-core.md, 07-displays.md, 08-physics.md, 11-game-framework.md,
14-authoring-guide.md, 15-launch-tables.md, 16-testing-ci.md.

This document is binding. Every flow, key, and number below is a requirement
unless it is explicitly marked as a per-table or per-setting override.

## 1. Vision

Tiltburst is a cross-platform digital pinball game that feels like a real
machine. It ships with five original tables and a table format that an LLM can
author end to end in plain text.

### 1.1 Product pillars (priority order)

1. **Feel first.** Flipper response, ball weight, nudge, and tilt must behave
   like a well-maintained real machine. When any decision trades visual
   fidelity against feel, feel wins. The gate scenarios are: catch, cradle,
   backhand, post pass, dead bounce (see glossary §11 and 08-physics.md).
2. **Latency is a feature.** Input→sim < 4 ms, frames-in-flight 1, sounds
   scheduled from sim ticks. Every subsystem is instrumented from its first
   milestone (see 02-decisions.md ADR-011 and ARCHITECTURE.md §3).
3. **LLM-authorable content.** A complete table — layout, rules, art, music —
   is four plain-text files. If a feature cannot be expressed in text, it does
   not ship in the table format (02-decisions.md ADR-008).

## 2. Reference hardware profiles

### 2.1 Profile A — cabinet (primary)

A 1960s pinball cabinet gutted and converted. This is the machine the game
must be excellent on.

```
        side view                     player's view of displays
   ______________
  |  backglass   |  ~square TV      playfield TV (portrait):
  |   TV (~1:1)  |                  1080 wide x 1920 tall, 60 Hz.
  |______________|                  The OS may report it as
  /              /                  1920x1080 landscape (physically
 /  playfield   /   1080p TV,       rotated TV) - Tiltburst rotates
/   TV portrait/    lying flat,     in the projection matrix, never
|_____________|     60 Hz           via OS display rotation.
 [flipper btn]      [flipper btn]
      [start] [plunger btn]
```

- OS: Windows 10/11. Single mid-range GPU (GTX 1650 class or better).
- Playfield: 1080p TV, portrait, 60 Hz. Backglass: ~square TV, 60 Hz.
- Input: cabinet buttons wired to a keyboard encoder (iPac class). The game
  receives them as plain keyboard scancodes — there is no gamepad, no
  accelerometer, no analog plunger in v1 (02-decisions.md ADR-014).
- Audio: powered speakers on the PC's default output.

### 2.2 Profile B — desktop (secondary)

Any Windows/Linux/macOS desktop or laptop, one display, any orientation,
60–240 Hz, keyboard input. This profile must be fully playable: single-display
fallback (canon §5.9) shows the playfield with a toggleable backglass overlay
(key `B`, §6). Windowed mode must exist for development (`--windowed` flag,
see 05-engine-core.md).

## 3. Requirements R1–R10, expanded

Each row is a testable statement. "Verification" names the objective check;
"Owner" is the milestone whose acceptance criteria include it (04-milestones.md).

| ID | Binding statement | Verification | Owner |
|----|-------------------|--------------|-------|
| R1.1 | On Profile A, playfield presents at 60 fps with zero missed vsync during a 5-minute `tb_autoplay` run on every shipped table | Frame-time ring buffer exported by the F1 overlay; assert max frame gap < 17.5 ms | M19 |
| R1.2 | Render loop sustains native refresh up to 240 Hz without slowing the 1000 Hz sim | High-refresh or IMMEDIATE-present run; sim tick counter must advance 1000 ± 1 ticks/s | M19 |
| R1.3 | Backglass can never stall the playfield | Debug command injects a 1 s backglass stall; playfield frame times unaffected | M12 |
| R2.1 | Flipper key edge → flipper torque applied in sim: **p99.9 < 4 ms over ≥ 10,000 scripted press edges** — percentile and sample count are one inseparable statement, and p99 is never the gate. **Measurement boundary** (canon §3 R2): the clock starts at the *OS-delivered* key edge (the SDL event's timestamp), so everything upstream of it is outside this number — on Profile A the cabinet buttons reach the OS through a keyboard encoder whose debounce and USB polling add ~1 ms on an iPac and up to ~8 ms on cheaper encoders. That budget is not ours to spend and the 4 ms target does not absorb it; the honest end-to-end figure is the photodiode `--latency-test` path (05-engine-core.md §14.4) | F3 latency overlay histogram exported from that same ≥ 10,000-press-edge scripted run; 04-milestones.md M4 (`Latency.InputToTickUnder4ms` + acceptance) and 05-engine-core.md Done-when state this sentence identically | M4 |
| R2.2 | Motion-to-photon < 25 ms on 120 Hz-class hardware | Instrumented software estimate (input timestamp → present timestamp + 1 refresh period) | M19 |
| R2.3 | At 60 Hz, software-estimated motion-to-photon ≤ 40 ms; input→sim budget unchanged | Same instrumentation on Profile A | M19 |
| R3.1 | Glow/bloom post pass present, per-table tunable, toggleable in settings (`render.bloom_enabled`, §7) | **F12 capture protocol** (06-rendering.md §15.1): a capture pair of the same Neon Drift frame with `render.bloom_enabled` true/false, plus a second pair showing the per-table tuning path (one primitive's `art.json` `glow.intensity` at two values — 13-art-direction.md §3.2; bloom is earned by glow, 06-rendering.md §12.1), reviewed against the 13-art-direction.md style checklist in the M13 PR — eyeball review, never pixel-gated (16-testing-ci.md §5). `tb_screenshot` is a stub until M15 and is **not** this row's evidence | M13 |
| R3.2 | Particle system sustains ≥ 2,000 live particles at 60 fps on Profile A | Headless gtest `perf_particles.two_thousand_live_at_60fps`: 2,000 live particles held for 600 update steps at `dt` = 16.67 ms (10 s of frames), asserting per-frame particle update + instance build stays inside the 06-rendering.md §17.1 CPU-encode budget of 1.5 ms (9.0 % of the 16.67 ms 60 Hz frame period) and the pool never allocates. On-hardware half: F12 capture of the F1 overlay showing ≥ 2,000 live particles at ≥ 60 fps. No `tb_autoplay` perf mode exists — 14-authoring-guide.md §8.2 is the normative tool contract and has no such flag | M13 |
| R3.3 | All five tables use the 13-art-direction.md palette and decal system | tb_validate art checks | M17 |
| R4.1 | Display auto-detection per canon §5.9 (portrait→playfield, squarest→backglass) | Unit tests feeding simulated display topologies to the heuristic | M12 |
| R4.2 | Manual override persists and beats heuristics | Integration test on `displays.json` | M12 |
| R4.3 | Single display ⇒ playfield only, `B` toggles backglass overlay | Manual protocol + UI test | M12, M18 |
| R5.1 | Five tables, each with complete layout, rules, art, SFX, music | `tb_validate` + `tb_autoplay` green on all five | M17 |
| R6.1 | 1–4 players alternating, correct rotation incl. extra balls | gtest suite on the game state machine | M10 |
| R6.2 | Duel mode (2-player, §5.2) complete | gtest + manual protocol | M18 |
| R7.1 | Feel scenarios (catch, cradle, backhand, post pass, dead bounce) pass as **FT-01…FT-08** on the 08-physics.md §5.6 feel rig | One gtest per scenario — `feel_scenarios.ftNN_*` (16-testing-ci.md §2.5) — on a rig built **in test code**: seed `0x54425354`, **state-triggered** input predicates ("press when `ball.y ≤ Y`"), **no `table.json` and no replay file** (`.tbreplay`/`.replay.json`) anywhere in the feel suite. Determinism half: `det_feel.twice_in_process_ft03`, which re-runs FT-03 twice in-process and compares `state_hash()` every 1,000 ticks. Bands are 08-physics.md §5.7's and are never widened here; FT-09/FT-10 are the M8 half of R7 | M4 |
| R7.2 | Every element type in canon §5.6 implemented and covered by at least one sim test | Test inventory check | M6–M8 |
| R8.1 | CI builds and tests green on Windows, Linux, macOS for every PR | GitHub Actions matrix | M0 |
| R8.2 | Packaged builds boot to attract on a clean machine per OS | M19 packaging checklist | M19 |
| R9.1 | A table pack with zero binary assets loads, renders, and plays | `test-lab` table has no `assets/` dir; loaded in tests | M5 |
| R9.2 | `tb_validate` rejects every file in the invalid-table test corpus with an actionable message | Corpus test | M15 |
| R9.3 | Atomic Diner is authored using only 14-authoring-guide.md and the tools | M16 journal evidence | M16 |
| R10.1 | Every milestone merged through the review loop of 03-process.md | JOURNAL.md audit | M20 |

**Hardware-fallback rule (R10).** Any verification row above whose check
runs "on Profile A" or otherwise requires the physical reference cabinet
(R1.1, R2.3, R3.2, and the §9 metrics measured there; sibling docs' own
hardware-bound checks follow the same rule) is **fallback-eligible**: if the
reference cabinet is unavailable to the implementor, run the same
instrumented protocol unchanged on the best machine available
(display-topology cases use the R4.1 simulated-topology harness), record the
result
as **PROVISIONAL-PASS** in the M20 audit, and append a JOURNAL.md note
listing the rows that await confirmation on the cabinet. A PROVISIONAL-PASS
satisfies the owning milestone's acceptance criteria. This is the documented
human-free fallback demanded by PLAN.md §2 ("never ask a human") and follows
the execute–journal–continue pattern of the 03-process.md §3.2 fallback
matrix. Split rows fall back only in their hardware-bound half: R3.2's
headless `perf_particles.two_thousand_live_at_60fps` gtest is
hardware-independent and must be green everywhere (no PROVISIONAL-PASS for
it); only its F12 on-cabinet frame-rate capture is fallback-eligible.

## 4. Player experience flows

All timings below are defaults; per-table overrides only where stated.
State-machine ownership: 11-game-framework.md.

### 4.1 Boot → attract

1. Launch `tiltburst`. Splash screen (logo on black) while subsystems start;
   target ≤ 2 s on Profile A.
2. Display detection and window creation per canon §5.9 / 07-displays.md.
3. Enter **Attract mode** (§4.8). No menu appears until the player presses a key.

### 4.2 Table select

1. From attract, **Start** (`1`/`Enter`) opens Table Select; flipper keys
   page the attract screens instead (§4.8).
2. Left/right flipper keys cycle through installed tables. The playfield
   display shows the table's showcase art; the backglass shows its name,
   high-score list, and a one-line description from `table.json` metadata.
3. **Start** confirms and loads the table. `Space` also confirms (§6).
4. **Escape** (short press) returns to attract. 60 s of inactivity returns to
   attract automatically.

### 4.3 Starting a game and adding players

1. With a table loaded and idle (table attract), **Start** begins a 1-player
   game: `game_start` fires, Player 1, Ball 1 of 3 (default; 3 or 5 in
   settings).
2. Each further **Start** press adds one player, up to 4, and is allowed from
   `game_start` until the **first `ball_end` event of the game** (i.e. during
   anyone's Ball 1 — in practice Player 1's). Presses after that, or beyond 4
   players, play a reject sound and do nothing.
3. Adding a player never interrupts the ball in play; the backglass updates
   the player roster immediately.

### 4.4 Ball flow

1. **Ball serve.** `ball_start` fires; the trough delivers one ball to the
   shooter lane. HUD shows "PLAYER n — BALL k".
2. **Plunge.** Hold `Space` to pull the plunger: power ramps 0→100 % over
   1.5 s of hold (curve in 08-physics.md), with an on-screen power gauge.
   Release launches. `ball_launched` fires when the ball leaves the shooter
   lane. A short tap gives a soft plunge — this must be possible, it is how
   skill shots are hit.
3. **Skill shot.** Each table defines a skill-shot target reachable only from
   the plunge (15-launch-tables.md). The skill-shot window is owned by each
   table's `rules.lua` (a `tb.timer` armed on `ball_launched`, 3–5 s by
   convention; see each table's §x.5 in 15-launch-tables.md) and also closes
   on the first flipper press. The
   award is decided by the **first `switch_hit` after launch** whose `tags`
   do not include `"button"`: it awards the skill shot when it comes from
   that table's skill-shot element, and closes the window otherwise. (Per
   10-scripting.md §4.1 every element actuation fires `switch_hit` first and
   its specialized event immediately after, and cabinet-button presses —
   flippers and launch — fire `switch_hit` with `tags = {"button"}`, so
   button switches never qualify.)
4. **Ball save.** Default: armed at `ball_launched` for 8 s (per-table
   override via `tb.ball_save`). If the ball drains while armed, it is
   auto-served and auto-plunged at full power, HUD shows "BALL SAVED", and the
   save does not re-arm for that ball unless the script re-arms it.
   `ball_save_expired` fires when the timer lapses.
5. **Nudge and tilt.** Each nudge key press applies one impulse to all balls
   (magnitudes in 08-physics.md) and adds to the tilt bob accumulator
   (11-game-framework.md). Crossing the warning threshold fires
   `tilt_warning` (default 2 warnings allowed); the next crossing fires
   `tilt`: flippers go dead, all scoring stops, the ball(s) drain, and the
   ball's bonus is forfeited. Tilt ends only the current ball, never the game.
6. **Drain.** Ball enters the outhole → `drain`. If other balls remain in
   play (multiball), play continues; the last ball draining triggers ball end.
7. **Ball end.** Bonus count (§4.5), then the next player in rotation is
   served (§5.1), or `game_end` if that was the last ball of the last player.
   Extra balls: the same player is served again immediately, no bonus reset
   rules change (11-game-framework.md).

### 4.5 Bonus count

At end of ball (`ball_end`):

1. Gameplay freezes; playfield dims; the bonus sequence plays on both
   displays.
2. The display shows "BONUS <bonus> × <multiplier>" (bonus accumulated via
   `tb.add_bonus`), then the product counts into the score in at most 25
   visible steps over 2 s, one tick sound per step; a zero bonus shows
   "NO BONUS" for 0.8 s instead (11-game-framework.md §4.5).
3. Holding both flippers collects the remainder instantly (total is
   identical).
4. On a tilted ball, "TILT" is shown for 1.5 s and no bonus is scored.

### 4.6 High-score initials entry

1. After `game_end` of a **standard game**, every player whose score enters
   the table's top-10 list enters initials, in ascending player number
   (11-game-framework.md §7). Duel scores never qualify (§5.2).
2. Entry UI: three slots, cursor on slot 1, every slot pre-showing `A`. The
   glyph ring, in cycle order: `A`–`Z`, `0`–`9`, space, `‹` (backspace).
   Right flipper = next glyph, left flipper = previous glyph; **Start** (or
   `Space`) confirms the slot and advances; confirming slot 3 commits the
   entry. Selecting `‹` and pressing Start moves back one slot; on a cabinet
   whose plunger button does not double as confirm, the plunger key is also
   backspace (11-game-framework.md §7, collision rule in its §8.1).
3. Holding a flipper key auto-repeats the cycle at 10 glyphs/s after a
   500 ms delay.
4. After 60 s without input, the currently displayed three glyphs are
   committed as-is — no blank padding, so an untouched entry commits `AAA`.
5. Scores persist per table (storage in 11-game-framework.md). The match
   feature (glossary) is **not implemented** — Tiltburst is free-play.

### 4.7 Game over → attract

After high-score entry (or immediately if nobody qualified), a "GAME OVER"
card shows final scores for 10 s. Start begins a new standard game on the
same table; a plunger press or the 10 s timeout returns to attract (§4.8),
whose playfield keeps showing this table with its attract light show
(11-game-framework.md T18/T19).

### 4.8 Attract loop

Attract pages cycle on the backglass (single display: on the playfield),
while the playfield display shows the last-selected table rendered with its
attract light show. Attract is **framework/art-driven**: the show is the
fixed 15 s framework routine of 13-art-direction.md §7.2, played on the wall
clock (no sim ticks, so it is outside the replay record) and parameterized
only by data the table already declares — its `art.json`, each `light`
element's `function` tag, and its `light_groups`. Attract music is
framework-selected (12-audio.md, 11-game-framework.md §8.2). There is **no
Lua attract hook and no attract-time `lua_State` in v1**: no `rules.lua` is
loaded and no script runs in Attract, and the table sim is released on entry
(11-game-framework.md §2.3, Attract row). Pages, per 11-game-framework.md
§8.2:

| Page | Duration | Content |
|------|----------|---------|
| Logo | 8 s | Tiltburst logo art, neon/particle choreography |
| High scores | 8 s | Top 10 of the last-selected table, GRAND CHAMPION highlighted |
| Rules card | 10 s | `meta.rules_card` text lines from the last-selected table |
| Press start | 5 s | "PRESS START" pulsing at 1 Hz + "1–4 PLAYERS · DUEL MODE" |

Flipper keys page manually and reset the page timer. Start exits to Table
Select (§4.2) — the keypress is consumed by the transition, never
interpreted as a game action. Attract music at −12 dB relative to game music
(12-audio.md).

## 5. Multiplayer UX

### 5.1 Alternating play (1–4 players)

- Rotation is strict: P1 B1, P2 B1, … Pn B1, P1 B2, … An extra ball repeats
  the same player before rotation continues.
- On player change, `player_up` fires and a "PLAYER n — YOU'RE UP" interstitial
  shows for 2 s (skippable by flipper). The backglass always shows all
  players' scores with the active player highlighted.
- All per-player script state lives in `tb.state` and is swapped automatically
  on player change (canon §5.7); drop-target banks, lights, and mode progress
  restore to that player's saved state per 11-game-framework.md.
- Tilt, ball save, and bonus are per-ball, per-player.

### 5.2 Duel mode (summary)

Duel is a 2-player, best-of-3-rounds head-to-head match on one table,
selected in the table-select mode sub-phase. Round r = ball r: each player
plays a timed ball of **75 s of live play** (the timer pauses while every
ball is in the plunger lane or held). A drain before timeout forfeits the
remaining time and simply ends the ball — there is no ball save in Duel; at
timeout the ledger freezes and the flippers die until the ball drains. The
higher round score wins the round (shown as a pip); exactly equal round
scores void the round. First to 2 round wins takes the match, otherwise more
pips after round 3 wins; if pips are still tied after round 3, each player
plays a 30 s sudden-death ball, repeated while the tie persists. Duel
scores are excluded from the high-score tables entirely (§4.6): 75 s scores
are incomparable with standard-game scores. Scripts cannot detect Duel —
`rules.lua` runs unchanged and every difference is framework-side. The full
specification — timer rules, HUD, and the
framework-side rule table — lives in 11-game-framework.md §3.4 and is owned
by M18.

## 6. Default input map

Bindings are by **SDL scancode** (physical key position), not keycode, so
cabinet keyboard encoders and non-QWERTY layouts behave identically. All
bindings are remappable in Settings (M18) and persisted in the user config dir
(05-engine-core.md). Names below are the US-layout labels of those scancodes.

| Action | Default | Behavior |
|--------|---------|----------|
| Left flipper(s) | `Left Shift` | Press = energize, release = drop. **All** left-side flippers on a table, upper and lower, share this key |
| Right flipper(s) | `Right Shift` | Same, right side |
| Plunger | `Space` | Hold to pull (power 0→100 % over 1.5 s), release to launch |
| Start / add player | `1` and `Enter` | Both scancodes are Start, always |
| Nudge left | `Z` | Shove on the cabinet's left side: balls get a +x (rightward) impulse |
| Nudge right | `/` | Shove on the right side: balls get a −x (leftward) impulse |
| Nudge up | `X` | Shove on the front: balls get a +y (up-table) impulse |
| Pause / back | `Escape` (press < 1.0 s) | In game: pause menu. In menus: back one level |
| Quit | `Escape` held ≥ 1.0 s | On-screen hold-progress ring; in game → confirm to table select; at main menu → quit app |
| Backglass overlay | `B` | Single-display mode only: toggle backglass overlay panel |
| Credit | `5` | Accepted for cabinet-encoder compatibility; cosmetic only (free play) |
| Perf/timing overlay | `F1` | Frame times, sim tick rate, per-stage budget |
| Physics debug draw | `F2` | Colliders, contacts, velocities, broadphase grid |
| Latency overlay | `F3` | Input→sim and motion-to-photon histograms |
| Event log overlay | `F4` | Scrolling sim/script event feed |
| Screenshot | `F12` | Writes `<SDL_GetPrefPath>/screenshots/tiltburst_YYYYMMDD_HHMMSS.png` (06-rendering.md §15.1). This is the R3.1/R3.2 evidence path at M13, before `tb_screenshot` exists |

Menu navigation: flipper keys move the selection (left/up, right/down), Start
or `Space` confirms, Escape goes back. Nudge keys are ignored in menus. The
overlays (F1–F4) exist in all builds, default off, and render on the playfield
display only; `F12` also exists in all builds and writes exactly one PNG per
press.

Direction convention for nudges (binding, because it is easy to invert): the
key names the side of the cabinet the player's hand strikes; the balls move
*away* from that side relative to the table. Impulse magnitudes and tilt-bob
accounting: 08-physics.md and 11-game-framework.md.

## 7. Menus and settings scope (v1)

Main menu: **Play** (table select) · **Duel** · **High Scores** · **Settings**
· **Quit**.

Settings pages (full spec in 11-game-framework.md, built in M18):

- **Display:** display assignment override (playfield/backglass per connected
  display), playfield rotation override (0/90/180/270°), backglass on/off,
  vsync/present-mode selection, frame cap (`video.max_fps`: 0 = match the
  display refresh rate, an integer caps it, −1 = uncapped), show refresh
  info, glow/bloom on-off
  (`render.bloom_enabled` — the R3.1 toggle) and bloom strength
  (`render.bloom_strength`). Exactly the `video`/`render` keys of the
  05-engine-core.md §11.1 settings schema, no others: a setting that is not
  in that schema does not exist.
- **Input:** remap every action in §6, plunger max-pull time (0.5–3.0 s,
  default 1.5 s), nudge level (1–3, default 2; settings key
  `input.nudge_level`, Δv per level in 08-physics.md §7.1).
- **Audio:** master / music / SFX volumes (0–100 %), output device, buffer
  size preset (see 12-audio.md).
- **Game:** balls per game (3/5), ball save on/off and duration (0–15 s,
  default 8 s), tilt sensitivity (warnings allowed 1–3, default 2), reset high
  scores per table.
- **Accessibility:** screen-shake on/off, flash-intensity reduction (caps
  full-screen flashes at 50 % brightness, 13-art-direction.md §12).
- **Debug:** overlay toggles, show determinism seed, autoplay demo start.

Settings persist immediately on change to the user config dir. No "apply"
button except display assignment (which prompts confirm-or-revert with a 10 s
timeout, reverting automatically — the player may be unable to see the new
layout).

## 8. Non-goals for v1

These are explicitly out of scope. Do not build them, stub them, or leave
hooks beyond what is listed.

- **Networked play** of any kind (02-decisions.md ADR-010).
- **VR / stereoscopic output.**
- **ROM emulation, licensed tables, or importing existing table formats**
  (VPX, FP, etc.) (ADR-007).
- **Physical cabinet I/O** — solenoid/contactor feedback, analog plunger
  encoders, addressable LEDs. *Designed for, not built:* the input and event
  abstractions must not preclude them (ADR-014), but no code ships.
- **Mobile platforms.**
- **A third display** (DMD/topper). v1 drives playfield + backglass only
  (02-decisions.md amendment table).
- **A GUI table editor.** Authoring is text + CLI tools (ADR-008).

**Post-v1 candidates (no spec, no scope, no hooks in v1):** a colorblind-safe
palette variant — 13-art-direction.md defines no such palette, so v1 ships
none and Settings (§7) exposes no toggle for it.

## 9. Success metrics

Measured before the M20 Definition-of-Done audit; all must pass.

| Metric | Target | Source |
|--------|--------|--------|
| Missed vsync on Profile A, 5-min autoplay, all 5 tables | 0 | R1.1 capture |
| Input→sim latency, measured from the OS-delivered key edge (encoder debounce and USB polling are outside this boundary — R2.1) | **p99.9 < 4 ms over ≥ 10,000 scripted press edges** (the percentile and the sample count are one gate; a p99 figure or an unstated sample count does not satisfy this row) | F3 overlay export of that run (R2.1) |
| End-to-end press→light on Profile A (the honest player-facing number: encoder debounce + USB polling + the row above + scanout) | Measured and filed; no numeric gate — the gate is the row above, and this row passes by existing | Photodiode `--latency-test` CSV (05-engine-core.md §14.4) |
| Sim tick cost p99 on Profile A | < 500 µs (half the 1 ms budget) | F1 overlay export |
| Audio output latency | < 10 ms | 12-audio.md instrumentation |
| Determinism suite | 100 consecutive green runs per platform | 16-testing-ci.md |
| Median ball time per table (`tb_autoplay --seconds 300`, skill 1) | 22–60 s (`ball_time_s.p50`) | 14-authoring-guide.md §8.3 metrics |
| Crash bugs known at release | 0 | issue tracker |
| New table authored from 14-authoring-guide.md alone | 1 (Atomic Diner, M16) | JOURNAL.md |

Metrics that name Profile A are fallback-eligible per the §3
hardware-fallback rule: when the reference cabinet is unavailable, measure
on the best available hardware and record PROVISIONAL-PASS with a
JOURNAL.md note.

## 10. Glossary of pinball terms

Written for a reader who has never seen a pinball machine. Diagram first;
terms reference it. This glossary is the vocabulary standard for all docs,
code identifiers, and table-authoring output.

```
        top of table (away from player)
  _________________________________________
 |  [top lanes] o o o      ___             |
 |   (rollovers)          /pop\  bumpers   |
 |  __                    \___/ o o        |
 | |ramp \___            _________     s   |
 | |  entrance\         |drop bank|    h   |
 |  \          \         " " " "       o   |
 |   \  orbit   \                      o   |
 |    |  (loop    \      [spinner]     t   |
 |    |   around   |        ||         e   |
 |    |    edge)   |     [standup]     r   |
 |    |            |                       |
 |   /   [kicker/scoop]                l   |
 |  |                                  a   |
 |  | slingshot  /\        /\  sling   n   |
 |  |           /__\      /__\         e   |
 |  | outlane | inlane   inlane | out | o  |
 |  |         |      \   /      | lane| <-plunger
 |   \        |  ____ \ / ____  |     |    |
 |    \       | /left\   /rght\ |    /     |
 |     \      | \flip/   \flip/ |   /      |
 |      \_____|__________ _____|___/       |
 |             [ drain / outhole ]         |
 |__________________[apron]________________|
        bottom (player stands here)
```

| Term | Definition |
|------|------------|
| Playfield | The sloped surface the ball rolls on. Tilted toward the player (default 6.5°), so gravity constantly pulls the ball down toward the flippers and drain. |
| Flipper | A player-controlled bat near the bottom of the playfield that rotates upward when its button is pressed to strike the ball. The core verb of pinball. |
| Slingshot | A triangular kicker above each flipper with a rubber face that fires the ball away when hit. A major source of chaotic ball motion. |
| Inlane | A lane that guides the ball from the sides safely onto a flipper. Good. |
| Outlane | A lane outside the inlane that leads past the flippers to the drain. Bad. Ball in the outlane is almost always lost. |
| Drain / outhole | The gap between the flippers (and the outlane exits) where the ball leaves play. "Draining" = losing the ball. |
| Trough | The hidden channel under the playfield that stores balls and serves the next one to the shooter lane. |
| Apron | The decorated cover over the drain/trough area at the bottom of the playfield. |
| Shooter lane / plunger lane | The channel on the right edge where a new ball waits to be launched. |
| Plunger | The spring rod (here: a held button) that launches the ball from the shooter lane; hold longer = harder launch. |
| Plunge | The act of launching the ball with the plunger. A "soft plunge" is a deliberately weak launch. |
| Skill shot | A target that can only reasonably be hit by plunging at a precise power; rewards a controlled plunge at ball start. |
| Orbit / loop | A path around the outer edge of the playfield: ball enters one side, circles the top, exits the other side at speed. |
| Inner loop | A shorter loop through the middle of the playfield. |
| Ramp | An inclined path the ball climbs above the playfield surface, usually returning it to an inlane. In Tiltburst, a constrained 1-D path with a height profile (canon §5.6). |
| Wireform | A ramp return made of bent wire rails. In Tiltburst: a ramp with different art. |
| Upper playfield | A small second playfield raised above the main one, with its own flipper(s). In Tiltburst: a free 2-D area on `layer: 1`. |
| Pop bumper | A mushroom-shaped element that violently kicks the ball away on contact. Usually in clusters of 3. |
| Slingshot rubber / rubber | The elastic band material on posts and walls that makes the ball rebound lively. A material in Tiltburst, not an element type. |
| Post | A small round peg, often rubber-ringed, that deflects the ball. |
| Standup target | A fixed flat target that scores when struck and does not move. |
| Drop target | A target that drops below the playfield when hit, staying down until reset. |
| Drop target bank | A row of drop targets; completing (dropping) all of them usually awards something and resets the bank. |
| Spinner | A hinged plate in a lane that spins when the ball passes through, scoring per revolution. |
| Rollover | A switch in a lane or button in the playfield that scores when the ball rolls over it. |
| Lane change | Pressing a flipper button to rotate which top-lane lights are lit, letting the player complete lane sets deliberately. |
| Gate | A one-way flap: the ball can pass in one direction only. |
| Kicker | Any solenoid device that propels the ball: slingshots, kickbacks, scoops, VUKs. Also the Tiltburst element type for hole-kickers. |
| Scoop / saucer | A recessed hole that captures the ball, then kicks it back out after the game decides what to award. |
| VUK (vertical up-kicker) | A kicker that fires the ball vertically, typically onto a ramp or wireform. |
| Kickback | A kicker in an outlane that saves the ball by firing it back up the playfield. Usually one-shot until relit. |
| Captive ball | A ball permanently trapped in a short lane; the player's ball hits it to transfer momentum and score. |
| Ball lock | A mechanism that holds the player's ball (a "locked" ball) to build toward multiball; a new ball is served to keep playing. |
| Multiball | A phase with 2+ balls in play at once. High scoring, high chaos. |
| Jackpot | The big scheduled award, usually available during multiball. |
| Mode | A timed or objective-based sub-game with its own rules and shots (e.g. "complete 3 ramps in 30 s"). |
| Wizard mode | The climactic mode awarded for completing all/most other modes; the table's finale. |
| Hurry-up | An award that counts down in value in real time until collected or lost. |
| Combo | Hitting a defined sequence of shots back-to-back within a short window for escalating value. |
| Ball save | A grace period after launch: if the ball drains, it is returned for free. |
| Extra ball | An earned award: the player plays an additional ball without passing the turn. |
| Replay | A free game awarded for beating a score threshold. In free-play Tiltburst: cosmetic fanfare only. |
| Match | End-of-game random digit match that awards a free game on real machines. Not implemented in Tiltburst (§4.6). |
| Bonus | Score accumulated during a ball and awarded at ball end (unless tilted). |
| Bonus multiplier | Multiplies the end-of-ball bonus (2X, 3X…), usually earned via lane completions. |
| Playfield multiplier | Temporarily multiplies *all* scoring, not just bonus. |
| Nudge | Physically shoving the cabinet to influence the ball. In Tiltburst: the impulse keys `Z`/`X`/`/`. Legal in moderation. |
| Tilt | The penalty for nudging too much: flippers die and the ball's bonus is lost. Detected by the tilt bob. |
| Tilt bob | The pendulum-and-ring sensor in real machines that detects excessive nudging. Simulated as an accumulator (11-game-framework.md). |
| Tilt warning | A free strike before tilt; Tiltburst default allows 2 warnings, the 3rd trigger tilts. |
| Slam tilt | A violent-abuse switch that ends the whole game on real machines. Not simulated in Tiltburst. |
| Cradle | Holding the flipper up so the ball comes to rest on it — full control, the basis of aimed shots. |
| Live catch | Raising the flipper to meet an incoming ball and deadening it into a cradle without a bounce. |
| Dead bounce | Letting a ball bounce off an un-energized (down) flipper to the other flipper, killing its speed. |
| Post pass | Passing the ball from one flipper to the other by bouncing it off the slingshot post with a flick. |
| Backhand | Shooting a target on the same side as the flipper holding the ball (a steep, cramped angle). |
| SDTM ("straight down the middle") | A drain straight between the flippers, where neither can reach. The most feared outcome of a shot. |
| Magna-save | A player-triggered magnet near an outlane that yanks the ball back from draining. |
| Magnet | An under-playfield electromagnet that grabs, holds, or flings the ball; used for toys and mechanics. |
| Insert | A shaped light embedded flush in the playfield indicating a rule state ("SHOOT AGAIN", arrows). Tiltburst element type `light`. |
| Toy | A decorative or interactive prop (a model, a crane) that is the table's centerpiece. Element type `toy`. |
| Attract mode | The self-running light/score/art show a machine plays when idle, to attract players. In Tiltburst it is framework/art-driven — no script hooks (§4.8). |
| Backglass | The upright decorated panel at the back showing art and scores. In Tiltburst: the second display. |
| EOS (end-of-stroke) | The point/switch at full flipper extension; real flippers switch to a weaker "hold" coil there. Governs cradle strength in 08-physics.md. |

## 11. Common pitfalls

Mistakes an implementor unfamiliar with pinball is most likely to make.

- **Nudge implemented as moving the ball.** Wrong. A nudge is one impulse
  event applied to *every* ball in play plus a tilt-bob accumulation; the
  playfield camera may shake cosmetically but geometry never moves.
- **Inverted nudge directions.** The key names the struck side; balls move
  away from it (§6). `Z` (left) ⇒ +x ball impulse. Write the sign test first.
- **Separate keys for upper flippers.** All left-side flippers share
  `Left Shift`; all right-side share `Right Shift`. Real machines wire them
  together. Never add a third flipper button.
- **Plunger as a fixed-power tap.** The plunger must be hold-to-charge with a
  visible gauge; soft plunges are required for skill shots (§4.4.2).
- **Tilt ends the game.** Tilt ends the *ball* (and forfeits its bonus). The
  game continues with the next player/ball.
- **Players addable at any time.** Only until the first `ball_end` of the
  game, max 4 (§4.3).
- **Ball save loops forever.** A saved ball does not re-arm its own save
  (§4.4.4); scripts must opt in to re-arming.
- **Attract mode swallows or double-handles Start.** The first Start in
  attract must transition to table select and be consumed — it must not also
  start a game or be dropped.
- **Writing a Lua attract hook.** Attract is framework/art-driven: no attract
  hook, no attract-time `lua_State`, no `rules.lua` running (§4.8). A table's
  attract look comes from `art.json` plus framework choreography — if you find
  yourself keeping a script alive "just for the light show", stop.
- **Bonus paid on tilt.** A tilted ball scores no bonus; the bonus sequence
  shows "TILT" and collects nothing (§4.5).
- **Escape quits instantly.** Short press = pause/back; only a held ≥ 1.0 s
  press (with visible progress) quits. Accidental quit destroys a 4-player
  game.
- **Binding by keycode.** Bind by scancode (§6) or cabinet encoders and
  AZERTY keyboards break.
- **Implementing match/coins/slam tilt** because the glossary mentions them.
  The glossary defines vocabulary; §4 and §8 define scope.
- **High-score entry only via keyboard text input.** Entry must work with
  exactly three cabinet buttons: two flippers + Start (§4.6).
- **Reporting the latency gate as p99.** The single binding statement is
  p99.9 < 4 ms over ≥ 10,000 scripted press edges (R2.1, §9), and the sample
  count belongs in the same sentence as the percentile. A p99 figure over a
  short burst hides exactly the tail this gate exists to catch.
- **Quoting the 4 ms gate as button-to-sim latency.** It is measured from the
  OS-delivered key edge (R2.1); the encoder's debounce and USB polling —
  ~1 ms on an iPac, up to ~8 ms on cheap encoders — happen before that clock
  starts and cannot be tuned from inside the process. Never "budget" for them
  by tightening the 4 ms, and never present the F3 export as what the player
  feels: that number comes from the photodiode `--latency-test` run
  (05-engine-core.md §14.4, §9).
- **Recording the feel scenarios as replay files.** R7.1 is proven by
  FT-01…FT-08 on the 08-physics.md §5.6 rig — code, fixed seed,
  state-triggered predicates — not by a recorded-input tape. A tape pins
  each press to an absolute tick, so every physics constant tuned later
  quietly turns the scenario into a different test (16-testing-ci.md §2.5).
- **Proving R3 with tools that do not exist yet.** `tb_screenshot` and
  `tb_autoplay` are stubs until M15, and `tb_autoplay` never gains a perf mode
  (14-authoring-guide.md §8.2 is its normative contract). R3.1/R3.2 are proven
  at M13 by the F12 capture protocol and the headless
  `perf_particles.two_thousand_live_at_60fps` gtest (§3).

## 12. Done when

- [ ] Every row of the §3 table has a passing verification artifact linked
      from its owning milestone's PR, produced by the mechanism that row
      names — in particular R3.1/R3.2 are closed at M13 by the F12 capture
      pair (bloom on/off) and a green
      `perf_particles.two_thousand_live_at_60fps`, with no dependency on
      `tb_screenshot` or `tb_autoplay`.
- [ ] R2.1 evidence is reported as p99.9 < 4 ms over ≥ 10,000 scripted press
      edges, measured from the OS-delivered key edge — the same sentence in
      04-milestones.md M4, 05-engine-core.md Done-when, and the PR text; no
      p99-over-a-burst figure anywhere, and no claim that this number is what
      the player's finger feels (that is the §9 photodiode row).
- [ ] Booting to Attract and idling through a full page cycle (§4.8) creates
      no `lua_State` and calls no script handler.
- [ ] All §4 flows demonstrable end to end on Profile A (or under the §3
      hardware-fallback rule): boot → attract → select → 4-player game with
      skill shot, ball save, tilt, bonus count, high-score entry → attract,
      using only the §6 default keys.
- [ ] §6 input map is the shipped default; every action remappable; bindings
      are scancode-based and persist across restarts.
- [ ] Single-display mode plays fully with `B` overlay toggle.
- [ ] Duel mode matches §5.2 and 11-game-framework.md.
- [ ] Settings pages exactly match §7 scope (no extras, nothing missing);
      display-assignment change has confirm-or-revert with 10 s timeout.
- [ ] All §9 metrics measured and green (PROVISIONAL-PASS per the §3
      hardware-fallback rule where the cabinet was unavailable), artifacts
      stored for the M20 audit.
- [ ] Nothing from §8 non-goals exists in the codebase beyond the permitted
      abstraction seams (ADR-014).
- [ ] Glossary terms are used consistently in all sibling docs and code
      identifiers (spot-check: no synonyms like "paddle", "bumper pad",
      "gutter" in code).
