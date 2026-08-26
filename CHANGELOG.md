# Changelog

All notable changes to Tiltburst are documented here. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: the
project versions the product, not the library API; v1.0.0 is milestone M20.

## [Unreleased]

### Added

- M05: table.json loader with prefab expansion (flipper_pair_standard,
  plunger_lane incl. merged variant, sling_pair, inlane_outlane_pair,
  orbit), sim builder with path/material baking, plunger charge-release
  sim, outhole drain + trough serve, `tables/test-lab` and the Neon Drift
  greybox, `--table` boot, F5 hot-reload, and perf_tick.gate_tables.

### Fixed

- M05: live-catch window restored to the documented 50 ms default
  (merged M4 carried 70 ms; ADR-021 text and code now agree).

### Added

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

