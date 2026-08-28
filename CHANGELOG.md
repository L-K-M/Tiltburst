# Changelog

All notable changes to Tiltburst are documented here. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: the
project versions the product, not the library API; v1.0.0 is milestone M20.

## [Unreleased]

### Added

- M11: audio engine (12-audio.md) — miniaudio device with the period
  ladder and §2.2 startup log, allocation-free 32-voice mixer with
  priority/age stealing + per-patch cap, the sfxr-style SFX synth
  (44.1 kHz generation, −1 dBFS normalization, 48 kHz output), the
  drift-corrected tick→sample clock with sample-accurate scheduling,
  audio.json patches/wav/map with the 24-patch built-in bank, all 19
  automatic §7.2 purposes fired from the sim with impact velocity and
  position pan, tb.play_sound, framework sounds, and --audio-null /
  --audio-latency-test. tables/test-lab ships its §6.1 audio.json.
- M10: game framework (11-game-framework.md §2–§7) — the GameState
  machine running in phase 3 of the sim tick, 1–4 player rotation with
  per-player tb.state swap, extra balls/replay, nudge physics with the
  tilt bob + abuse accumulator (two warnings then tilt), framework
  tilt consequences (flippers/coils dead, ledger frozen, every held
  ball force-ejected), ball save, multiball edges, bonus collection,
  and per-table top-10 high scores with initials entry and
  meta.default_scores seeding (neon-drift ships themed defaults;
  tables without them start empty).
- M09: sandboxed Lua script host (10-scripting.md §1–§4) with the complete
  canon §5.7 event + action surface, instruction watchdog, per-player
  tb.state, tick-granular timers, latched physical actions, and the
  BackglassModel; tables/test-lab/rules.lua ships and Neon Drift gains
  its M9 element roster + rules v1.

- M08: ramps as constrained 1-D paths with derived seam layers and
  bidirectional layer transfer, layer masks, and magnets with the §6.12
  field — completing the R7 element set (ramp, magnet, toy parse; FT-09
  and FT-10 green).

- M07: kickers, drop target banks, captive balls, and ball locks per
  08-physics.md §6.5/§6.9/§6.13/§6.14, plus the ball-save mechanism and
  the active+trough+locked ball-accounting invariant.

- M06: slingshots, pop bumpers, standup targets, rollovers, gates, and
  spinners per 08-physics.md §6.2–§6.8, with switch_hit pairing, cooldowns,
  and the spinner plate model (per-revolution spinner_spin events).

- M05: table.json loader with prefab expansion (flipper_pair_standard,
  plunger_lane incl. merged variant, sling_pair, inlane_outlane_pair,
  orbit), sim builder with path/material baking, plunger charge-release
  sim, outhole drain + trough serve, `tables/test-lab` and the Neon Drift
  greybox, `--table` boot, F5 hot-reload, and perf_tick.gate_tables.

- M04: flipper stroke state machine, moving-capsule CCD, surface-velocity
  impulses and live catch; FT-01…FT-08 feel scenarios on the §5.6 code rig.
- M04: raw input layer (05 §9) — SDL/WinRaw/evdev producers over per-source
  edge rings with the late-latch contract (≤ 64 edges/tick, sub-tick tap
  survival, atomic reconciliation), focus gating, and nudge plumbing.
- M04: latency instrumentation (05 §14) — input→latch histogram backing the
  R2.1 gate, per-stage record ring, F3 overlay page, `--latency-test` mode
  with CSV export.

### Changed

- M04: restitution curve gains a low-speed cliff and the resting-contact
  cutoff lands at 0.15 m/s (ADR-021); caught balls settle instead of
  rattling at the sweep threshold.

### Fixed

- M10 (post-review hardening): multiplayer SHOOT AGAIN rotation (the
  extra ball is consumed where the pointer decision is made), tilt
  force-ejects energized magnets on de-energize, score-0 games never
  qualify for initials (a persisted 0 would have wiped the file on the
  next boot), corrupt score files keep a .bad copy and never lose the
  only copy on a failed keep, single-key physics.tilt tables keep the
  warn/hard pair ordered, out-of-range nudge levels fall back to the
  middle strength, and the golden recorder/compare paths share one
  replay helper with provenance and full 30-sample coverage.
- M09: §3.8 push-out now depenetrates arcs (a 7 m/s orbit ball could
  tunnel through the corner-arc wall band; det_golden regenerated).
- M09: inlane_outlane_pair outlane mouth widened to 32.8 mm passable
  (ADR-024; the old default wedged balls in §6's jam band).
- M05: live-catch window restored to the documented 50 ms default
  (merged M4 carried 70 ms; ADR-021 text and code now agree).
- M04: resting/tangential contacts within kSkin now resolve as persistent
  contacts — friction acts every tick on sliding balls instead of only at
  normal crossings (ADR-021 context).

### Added

- M03: renderer v1 — SDF primitive pipeline (circle/ring/rbox/capsule/arc/
  ball with stroke and glow), RGBA16F scene target, rotated letterbox
  present pass with piecewise sRGB encode, F2 debug draw of colliders and
  balls, F12 screenshots, and projection math unit tests.
- M02: headless simulation core — analytic CCD (segment/point/arc/ball
  pair), uniform-grid broadphase, shared-timeline TOI loop with push-out,
  §4.1 impulse contacts with velocity-dependent restitution, ball-ball
  collisions, state_hash with event-sequence accumulator, .tbreplay
  recorder/player, JSON tape loader, allocation-free hot path, layout
  guard, and `perf_tick.gate_synthetic`.
- M01: application skeleton — 1000 Hz sim thread with triple-buffered
  snapshots, SDL3 GPU device (frames-in-flight 1, MAILBOX-else-VSYNC),
  quad-pipeline overlay from committed shader blobs, settings.json with
  crash-safe writes, PCG32, `--render-smoke` offscreen path, and the
  §2.1 exit-code contract.
- M00: CMake/vcpkg build scaffold with all canonical targets, CI workflow
  (3-OS matrix, format, ASan, perf gates), `tiltburst --version`, tool
  stubs, first tests, vendored OFL fonts with provenance and SHA-256 pins.

### Fixed

- M00: Windows preset generator follows the runner image's Visual Studio
  2026 (ADR-017); tb-setup installs the autotools required by vcpkg's
  libxcrypt port (ADR-018); vendored `third_party/` headers exempt from the
  clang-format gate (ADR-016).

