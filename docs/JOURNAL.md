# Tiltburst Journal (append-only)

Newest entries at the bottom. Never edit or delete past entries;
corrections are new entries. Format: 03-process.md §3.1.

## M00 — Repository scaffold & CI (2026-08-25)

- Shipped: CMake scaffold with all canonical targets, vcpkg manifest
  (baseline 00c5775211f45cd08b37fce0484b4cb940e422ab), CMakePresets,
  tb-setup composite action + build-test.yml workflow (format / build-test ×3 OS /
  asan / perf-gates / weekly-deep / det-soak), tiltburst --version +
  exit-2 catch-all, three tool stubs, unit_scaffold.sanity,
  FontAssets.VendoredFontsPresentAndParse, vendored fonts + picosha2,
  CHANGELOG/JOURNAL seeds.
- Deviations:
  - Orbitron static Bold no longer exists upstream; vendored the variable
    font would violate 13-art-direction.md §5.1, so the orbitron role is
    filled by Chakra Petch Bold per §5.1's substitution clause → ADR-015.
    Logical font name stays `orbitron`.
  - picosha2.h is MIT-licensed upstream (plan said "public-domain");
    license recorded verbatim in tests/third_party/SOURCES.md.
- New ADRs: ADR-015, ADR-016, ADR-017, ADR-018.
- Worries: local dev box has no sudo; CI is the primary build gate for
  Windows/macOS until a machine with MSVC/Xcode is available.
- Mid-milestone: first CI run red — windows-latest migrated to VS2026
  (generator fixed per ADR-017) and Linux jobs lacked the autotools the
  libxcrypt port autoreconfs with, plus its libltdl-dev (ADR-018). Both
  spec docs amended in this PR per 03-process.md §3.3. Second run: Windows
  built but FontAssets failed on a CRLF checkout of SHA256SUMS — parser
  strips CR and .gitattributes pins vendored bytes / LF for the sums file.

- Mid-milestone: branch protection PUT on main returned 404 (token lacks
  admin; 2026-08-25). Fallback per 03-process.md §3.2: the §4 per-PR
  CI-green checklist enforces merges; six contexts confirmed verbatim from
  gh pr checks on PR #2. Retry once at M01.

## M01 — App skeleton & fixed-timestep loop (2026-08-25)

- Shipped: core (time/log/rng/config/assert), sim (SimSnapshot +
  TripleBuffer, 1000 Hz SimThread with §6.1 clamp), platform (CLI §2,
  SDL boot §1 subset, window, GPU device w/ FIF=1 + MAILBOX-else-VSYNC),
  render (renderer.h per 06 §2, quad pipeline from committed blobs,
  stb_easy_font overlay, --render-smoke offscreen path), 14 unit tests.
- Deviations:
  - Timebase unified as tb::now_ns() in tb_core over CLOCK_MONOTONIC/QPC
    (05 §3 said SDL_GetTicksNS; 04 M1 said ticks_now_ns — neither fits the
    layering) → ADR-019; both spec snippets amended same PR.
  - SDL_shadercross has no release tags; FetchContent pins commit
    e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba instead (ADR-012's "fixed
    release tag" applied as fixed commit).
  - Blobs generated locally: dxc v1.8.2505 → SPIR-V, spirv-cross → MSL;
    DXIL deferred (needs Windows DXC); CI shader compile stays OFF until a
    runner-provisioned DXC exists → committed-blob path per ADR-012.
    Windows/macOS render-smoke will log-skip until DXIL lands; Linux
    exercises lavapipe.
  - --headless runs a bounded 5000-tick probe then exits 0 so CI can
    verify display-less boot; unbounded headless sessions arrive with
    --replay (M4+).
  - Branch protection retry: PUT still 404 (no admin) at M01 open; fallback
    continues per §3.2.

## M02 — Headless simulation core & determinism (2026-08-25)

- Shipped: Vec2/Ball/materials/colliders, analytic sweeps (§3.2–§3.4, §8)
  with the five §2.1 worked examples pinned as unit tests, uniform-grid
  broadphase (§3.7) with brute-force property test, shared-timeline CCD
  loop + pull-back + push-out (§3.6/§3.8), impulse contacts with
  velocity-dependent restitution and spin transfer (§4.1–§4.3), energy
  property test, ball-ball collisions (§8), state_hash with the rolling
  event-sequence accumulator (16 §2.4.1), .tbreplay writer/reader (05 §13),
  JSON tape loader in test support, SimEvent ring skeleton, allocation-free
  step proven by a global allocator hook, layout include guard,
  perf_tick.gate_synthetic green (median-of-3 mean ≈ 1.8 µs, p99 ≈ 2.7 µs —
  far inside the 100/200 µs CI limits).
- Deviations:
  - det_golden.m2_bounce records/compares per-OS goldens; only the linux
    golden is committed (no Windows/macOS host available). Other OSes SKIP
    until their goldens land via CI artifacts. ADR-013 same-OS rule intact.
  - The m2_bounce tape's table_slug ("test-lab") is not yet validated —
    table loading arrives at M5.
  - sweep_circle_vs_point root-finding runs in double internally: the §2.1
    worked-example tolerances (1e-6 on normalized normals near-tangent)
    are unreachable in float without contradicting the spec's own decimals.
  - Broadphase brute-force test asserts no-misses (+dedupe/sort), not set
    equality: cell-level broadphase is conservative by design.
  - Branch protection retry at M02 open: PUT still 404 (no admin);
    fallback continues per §3.2.
- Golden regeneration (M02 review cycle): the tape loader's bit→action map
  was fixed to shift bits 6–9 down one (05 §13.1), changing the m2_bounce
  button stream; tests/golden/determinism/linux/m2_bounce.hashes
  regenerated per §2.4.4.

## M03 — Renderer v1 & debug draw (2026-08-26)

- Shipped: SDF primitive pipeline (sdf.vert/frag per 06 §8: circle, ring,
  rounded box, capsule, arc, ball; analytic AA, fill/stroke/glow), RGBA16F
  scene target sized by the §6.2 fit math, present pass (§12.5 pass F
  bloomless variant) drawing the rotated letterbox quad with piecewise sRGB,
  F2 debug cycle (colliders → +broadphase placeholder), F12 screenshots via
  offscreen re-render (never swapchain copy, §15.1), ViewTransform pure-math
  unit tests (corner mapping rot90, letterbox bars).
- Deviations:
  - Bloom chain, TBArt, real text stay out until M13 per milestone split;
    present pass is the bloomless variant of §12.5.
  - F2 broadphase-grid page renders nothing yet (grid data not published to
    the snapshot debug section; arrives with the table loader at M5).
  - Debug draw reads colliders from a render-side mirror scene rather than
    "the snapshot's debug section" — the snapshot debug section named by
    06 §16.1 does not exist in the plan (ledger defect #4); RenderFrame
    delivery per 04 M3 wins.
  - Local box has no Vulkan loader; render smoke verified on CI lavapipe.

## M04 — Flippers, low-latency input & latency overlay (2026-08-26)

- Shipped: flipper stroke state machine (REST/RISING/HOLD/DROPPING),
  moving-capsule CCD (conservative advancement + static fast path),
  solver integration with dynamic colliders, surface-velocity impulses,
  live catch; FT-01…FT-08 on the normative §5.6 code rig;
  det_feel.twice_in_process_ft03; det_replay.flipper_tape_hash_stable
  (+ committed tape + linux golden); HotPath.NoAllocationsWithFlippers;
  raw input layer per 05 §9 (SDL/WinRaw/evdev producers, edge rings,
  late-latch contract, focus gate, §9.8 suppression at the SDL source,
  nudge bits plumbed); latency instrumentation per 05 §14 (cumulative
  input→latch histogram = the R2.1 gate, per-stage record ring, F3
  overlay page, --latency-test mode with CSV + histogram export);
  perf_latency.input_to_tick_p999 green locally (p99.9 ≈ 0.06 ms over
  12,000 edges — synthetic same-tick latching).
- Deviations / new ADRs:
  - ADR-021: restitution curve gains a low-speed cliff and kRestSpeed
    lands at 0.15 m/s (supersedes ADR-020's interim 0.05). A flat e to
    the cutoff self-sustains a micro-bounce limit cycle on caught balls —
    FT-02's rattle. Live-catch constants return to §1.3 defaults
    (50 ms / 0.15); the WIP's undocumented 70/0.10 move is reverted.
  - ADR-021 context fix: resting/tangential contacts within kSkin now
    resolve as persistent contacts (immediate TOI), so friction acts
    every tick — previously sliding balls free-fell between rare normal
    crossings and rattled at ~0.10–0.13 m/s.
  - ADR-022: FT-04/06/08 scenario contracts re-scoped to rig-feasible
    outcomes. The §5.6 inlane walls form a pocket that captures any
    resting ball; wedge kinematics cap crook ejections near 1.2 m/s
    (below FT-04's ≥2.19 m/s-equivalent crossing, above FT-06's ceiling)
    regardless of constants — verified by kinematic analysis plus an
    empirical sweep of every §5.8 knob combination. Original bands kept
    for FT-01/02/03/05/07; power shots stay covered by FT-07.
  - Golden regeneration: m2_bounce hashes re-recorded (persistent
    contacts legitimately alter static-contact trajectories); new
    flipper_tap.hashes committed for the M4 tape.
- Mid-milestone: branch protection retry at M04 open: PUT still 404
  (no admin); fallback continues per §3.2.
- Reviewer-unavailable fallback taken at merge (03-process.md §2.7):
  review run 32979466497 concluded completed/cancelled at the workflow's
  own 90-minute job cap with zero output posted (no review, inline, or
  issue comment for any head SHA); an earlier run was manually cancelled
  at 74 min on a wrong zombie diagnosis (its frozen updated_at is normal
  for in-flight jobs — noted for future polling). Retrigger attempts
  (empty-commit pushes at acff58c and a4849a6, plus a PR close/reopen)
  created no runs at all — a GitHub Actions event-delivery outage
  (workflow_dispatch still worked). CI green for the merged tree was
  confirmed via a dispatched Build & Test run on a4849a6 (identical
  content; the only later commit is this journal note). Self-review
  checklist completed; findings fixed in the cleanup commit (dead WIP
  helpers, ADR reference numbers, F3 page in --latency-test).
- Worries: cradle-parking is sensitive to restitution shape (any future
  change to §4.2 must re-run the full FT suite); WinRaw source compiled
  but only exercisable on Windows runners; evdev replug path untested
  without /dev/input write access on CI.

## M05 — Table format v1 & Neon Drift greybox (2026-08-26)

- Shipped: table.json loader (nlohmann-json, comments on, JSON-pointer
  qualified TableLoadError) with the M5 schema — meta/playfield/physics/
  materials overrides + wall (point/arc paths), post, flipper, plunger,
  outhole, trough, light, plus parse-only slingshot/gate/rollover/
  pop_bumper/standup_target for prefab children and test-lab; prefab
  expansion engine (flipper_pair_standard, plunger_lane standalone +
  merged-variant detection, sling_pair, inlane_outlane_pair, orbit) with
  §5.1 expansion golden; sim_builder (path baking with arc nodes + corner
  caps, material table in SimState with per-table overrides); plunger sim
  per 08 §6.16 (charge curve, skill-shot bit-identical guarantee tested);
  outhole drain + basic M5 drain→serve loop; tables/test-lab (§7 listing)
  and tables/neon-drift greybox (M5 subset of 15 §1.3: orbit + merged
  shooter lane, 3 flippers, inlane/outlane pairs, sling bodies, inserts);
  --table boots in windowed and headless paths; F5 hot-reload (stop sim,
  rebuild, restart); lights drawn as debug circles; perf_tick.gate_tables
  (test-lab mean ≈ 1.0 µs, neon-drift ≈ 1.2 µs — far inside 100/200 µs).
- Deviations:
  - kLiveCatchWindowTicks drift fixed: merged M4 carried 70 ms while
    ADR-021/§1.3 say 50 ms; restored to 50 (feel suite green both ways —
    verified before choosing the documented value).
  - Determinism.NeonDriftGreyboxReplay runs the greybox twice in-process
    comparing hashes every 5k ticks (60 s total) rather than through a
    committed .tbreplay tape — the milestone's "stable hash" contract;
    tape-machinery coverage already exists via
    det_replay.flipper_tape_hash_stable.
  - M6+ element types parse but have no sim yet (milestone scope-out);
    their prefab children (gates, rollovers, slingshot faces, pops,
    standups) are inert until M6 lands.
  - Windowed greybox boot (F12 screenshot, F5 reload) is
    measured-on-non-reference: no local GPU/display; the load path is
    identical to the tested one and render-smoke covers lavapipe on CI.
- Mid-milestone: branch protection retry at M05 open: PUT still 404
  (no admin); fallback continues per §3.2.

## M04 — Flippers & input (WIP, unmerged)

- Branch milestone/M04-flippers-input carries: FlipperSim (08 §5.2 state
  machine), moving-capsule CCD (§3.5 conservative advancement + static
  fast path), solver integration (step 2, dynamic-collider sweep,
  surface-velocity impulses §5.3, live catch §5.4), generalized
  resolve_surface, FT rig + all eight M4 scenarios.
- Feel results: FT-01/03/05/07 green. FT-02/04/06/08 red.
- Diagnosis so far: caught-ball micro-rattle in the flipper/inlane pocket
  sustains |v| ≈ 0.10–0.13 (FT-02 band boundary); backhand/tap shots die
  or overpower depending on resume-from-drop timing. Suspected solver gap:
  resting/tangential contacts within kSkin get no friction (sweep fires
  only on normal crossings), so slide decay relies solely on discrete
  impacts. Next step: persistent-contact friction or §5.8 knob iteration
  (kRestSpeed/kSkin/live-catch window) with ADR + JOURNAL entries.

## M06 — Standard elements I (2026-08-26)

- Shipped: slingshots (face collider + §6.2 post-resolution kick, 80 ms
  cooldown, arm-visual counter), pop bumpers (§6.3 radial kick with
  deterministic rng_sim ±0.12 rad jitter, 60 ms cooldown, skirt flash),
  standup targets (§6.4 facing-side trigger, 100 ms debounce, passive
  rebound), rollovers (§6.8 capsule trigger with 0.012/0.016 m hysteresis,
  switch_hit + rollover pair), gates (§6.7 tri-state one_way/open/closed,
  blocking as absorbent-steel dynamic colliders via the pseudo-collider
  path, pass switches with 0.03 m re-arm, mechanical one_way flag),
  spinners (§6.6 crossing spin-up 25 rad/s per m/s, one-shot 0.12 m/s
  plate-inertia slowdown with transit hysteresis, slow-crossing steel
  wall, 0.55/s friction decay, per-revolution switch_hit + spinner_spin
  with instantaneous rpm). Static-contact log feeds reactive triggers;
  event pairing per the §6 preamble (switch_hit first). Loader gained the
  spinner type and the gate tri-state; builder bakes all six. Tests:
  Slingshot.FiresOnBandCrossing, PopBumper.KickVectorRadial,
  Standup.EmitsSwitchHitOnce, Gate.OneWayBlocksReverse,
  Rollover.TriggersAtOverlap, Spinner.SpinCountFromBallSpeed,
  Determinism.TestLabAllElementsReplay — 67/67 green; gate_tables
  perf holds (test-lab mean 0.91 µs, neon-drift 1.64 µs).
- Deviations: none — element physics per 08 §6.2–§6.8 verbatim; test-lab
  §7 listing untouched (its roster already covers pop/standup/rollover;
  slings and gates arrive via its prefabs).
- Mid-milestone: branch protection retry at M06 open: PUT still 404
  (no admin); fallback continues per §3.2.

## M07 — Standard elements II (2026-08-26)

- Shipped: kickers (§6.9 capture zone scan with saucer speed gate, CAPTURED
  dwell with the capture_ms auto-eject failsafe, eject vector, kicker_enter
  pairing); drop target banks (§6.5 facing-side contact triggers via the
  M6 contact log, collider disable on drop in BOTH find_earliest and
  pushout, per-target 120 ms drop / 250 ms raise animations, edge-guarded
  bank_complete, auto-reset timer, request_bank_reset for script control);
  captive balls (§6.13 1-D slot dynamics with end bounces, swept
  free-vs-captive contact with the admissible-motion impulse, strike
  switch_hit with 100 ms debounce, far-end full-travel event with 0.3 m/s
  arrival gate and 4 mm hysteresis); ball locks (§6.14 unconditional
  capture, switch_hit + ball_lock pairing with count payload, 3000 ms
  unclaimed auto-release failsafe, one-per-500 ms eject cadence); ball
  save mechanism (drain inside the window re-serves on the plunger, one
  save per window); ball accounting (ball_count/locked_balls on SimState,
  active+trough+locked == ball_count property-tested). Loader + builder
  for kicker/drop_target_bank/captive_ball/ball_lock. Tests:
  Kicker.CaptureDwellEject, DropBank.CompletesAndResets,
  CaptiveBall.StaysInLane, CaptiveBall.FullTravelEmitsOnceAfterSwitchHit,
  Trough.CountsNeverGoNegative, BallSave.TimerServesWithinWindow,
  MultiBall.ThreeActiveDeterministic — 75/75 green; table perf gate holds
  (0.97 / 1.73 µs mean).
- Deviations:
  - Kicker VUK style parses and stores but ejects like saucer/scoop in
    v1: ramp binding arrives at M8 with the ramp element itself.
  - Ball-lock claim plumbing (handler-consumed ball_lock events) arrives
    with Lua at M9; until then the 3000 ms unclaimed failsafe governs
    every capture, which is the conservative branch.
  - Drop-bank reset deferral (never raise into a ball) is checked at the
    animation level; the swept-overlap pre-check joins with tb_validate
    geometry at M15.
- Mid-milestone: branch protection retry at M07 open: PUT still 404
  (no admin); fallback continues per §3.2.
- Review cycles (GLM 5.3, 5 cycles to steady state): the significant
  catch was the inverted captive reaction-impulse sign — my tests had
  passed through end-clamp bounce chaos; confirmed with an isolated
  sign probe before the fix. Also: kicker capture payload speed,
  captive full-travel attribution, BallLockCapture rename (one broken
  intermediate commit where the rename missed events.h — CI caught it;
  lesson re-verified: never trust grep -c over build logs), eject-speed
  validation, cycle-3/4 nits. One declined with citation (release-timer
  reset: §6.14's 500 ms cadence scopes per release command).

## M08 — Ramps, layers & magnets (2026-08-26)

- Shipped: RampPath (§6.10.1 arc-length parametrized samples from the
  flattened path, per-end derived seam layers per §6.10.2 with the
  drop_exit internal-end rule); ramp dynamics (§6.10.4 gravity +
  profile-climb + linear damping + rolling resistance with the
  never-reverse rule; the 2-D arc length used as-is per spec); seam
  binding (alignment ≤ 50°, speed ≥ 0.1 m/s, strict pre/post crossing so
  an exiting ball never rebinds, 5% side-guide loss, correct far-end
  s_dot sign); exits (forward with ramp_made + switch_hit pairing and 5%
  loss, rollback out the entry, fall-back-off stall guard with the
  0.05 m/0.006 m window); magnets (§6.12 field with core clamp, rim fade,
  60 m/s² cap, pulse envelope, eddy damping); layer masks throughout
  (colliders, elements, seams); V010/V011 ramp profile validation at
  load; parse_path tracks the running position across chained arcs (V019
  half-chord check was reading garbage after an arc node); Neon Drift
  greybox gains drift_magnet + both ramps per 15 §1.3. Tests:
  Ramp.ClimbConservesPlausibleEnergy, Ramp.SlowBallRollsBack,
  Ramp.RidesDownFromLayer1, Layer.MasksIsolateColliders,
  Magnet.CaptureEnvelope, FT-09, FT-10 (make + rollback, bands
  unmodified), Determinism.FullElementSetReplay — 83/83 green;
  gate_tables holds (1.0/1.7 µs mean).
- Deviations / new ADRs:
  - ADR-023: §6.12 in-field eddy velocity damping 0.8 → 3.5 s⁻¹. At 0.8
    the braking cannot dissipate the rim-to-rim gravity gain (energy
    accounting + probe: every through ball escaped regardless of entry
    speed), so FT-09's catch band was unreachable. 3.5 is the smallest
    round value that holds with margin (2.0 escapes; 3.0 missed the 3 s
    |v| ≤ 0.35 band by 0.012 m/s). Spec text amended in this PR.
  - VUK kicker ramp-binding eject stays the M7 saucer-style eject: v1
    wire-up deferred with the ramp element's own edge cases; noted for
    M9 scripting integration.
- Mid-milestone: branch protection retry at M08 open: PUT still 404
  (no admin); fallback continues per §3.2.
- (Carried from M07 merge aftermath:) the M07 review-cycle history is
  recorded under the M07 entry above; this entry adds nothing further.

## M09 — Lua scripting & Neon Drift rules v1 (2026-08-27)

- Shipped: sandboxed Lua 5.4 + sol2 script host (10-scripting.md §1–§4):
  64 MiB capped allocator, fixed hash seed 0x74696C74, library whitelist
  (io/os/require/load/dump stripped, math.random raises, print → log,
  collectgarbage count-only), coroutine.create hooked to carry the §2.4
  watchdog (10k instructions/tick at 1k granularity; overrun disables the
  handler permanently and skips the tick's remaining invocations; §2.5
  10-consecutive-errors disable); the complete canon §5.7 surface — all
  22 events via tb.on, all 31 actions (4 tables: state/game/table_info/
  backglass; the rest functions), tb.state per-player proxy with swap,
  tb.timer/cancel_timer with freeze seam, BackglassModel with fixed
  buffers, tb.rng/rng_range on rng_script; physical actions latch to the
  next tick's phase 1 via ScriptAction + the solver's
  apply_script_actions; phase 2 dispatch from a per-tick emission-order
  event log + phase 4 timers/GC inside Solver::step; element-id string
  resolution over SimState::element_ids/element_tags. tables/test-lab/
  rules.lua ships verbatim per §6; neon-drift gains the full M9 element
  roster (speedo_spinner, drift_lock, gear_bank, pit_scoop, nos standups
  per 15 §1.3) + rules v1 (gears, drift mode, NOS, lock/multiball with
  the mandatory unlit-lock release, drain counting). App windowed +
  headless paths load rules.lua and begin_game. Tests: the six M9 suites
  + Determinism.ScriptRngReplayStable + NeonDrift.ScriptedGameReachesGame
  End (real table, real rules, 3 drains → game_end, score > 0) — 89/89.
  gate_tables now loads rules.lua (D16): test-lab mean 3.30 µs, neon-drift
  3.87 µs vs the 100/200 µs limits.
- Deviations / new ADRs:
  - ADR-024: inlane_outlane_pair divider top moved (0.062,0.268)→
    (0.074,0.262). The §5.4 numbers left a 23 mm outlane mouth (its own
    §6 jam band); a ball fed down the side wall wedged forever at
    (0.041,0.274) — observed live. Spec + prefab amended in-PR.
  - Arc push-out (§3.8): the M2 implementation skipped arcs in
    depenetration ("arc tips carry point colliders"); a 7 m/s orbit ball
    entering the corner-arc wall band (R−r < d < R+r) fell through BOTH
    §3.4 sweep gates and tunneled off-table (observed at
    (−18, −1216) m). Arcs now depenetrate with the angular-window check.
    Physics change → det_golden.m2_bounce regenerated per §2.4.4.
  - Ball-save uses/extra-ball caps, ball lifecycle events (ball_start/
    ball_end sequencing) are driven by tests through the fire_event
    framework seam; the real GameFsm owns them from M10.
  - tb.backglass.animate/play_sound/play_music record nothing yet —
    rendering/audio consumers arrive M11/M12; the calls validate args and
    no-op per spec ("a missing id means silence").
- Mid-milestone: branch protection retry at M09 open: PUT still 404
  (no admin); fallback continues per §3.2. CMakeLists clang-format
  incident: the WIP commit let clang-format run over CMake files AND
  dropped tb_sim's -ffp-contract=off determinism flag — caught by
  det_golden going red in a clean build; both repaired before the PR.
- Script-host segfault hunt (6 triage pushes, then the real fix): the
  macOS-arm64-only crash survived three plausible fixes (L luaL_error
  lambdas, watchdog-hook stack safety, handlers/timers released before
  lua_close) because x86/5.5 heap layout masked the defect. Pinning
  Lua 5.4.8 locally turned it into a reproducible Linux segfault; a
  signal-handler backtrace nailed it: ~unique_ptr<sol::state_view>
  calls luaL_unref on the state AFTER the destructor body's lua_close
  — a pure destructor-order use-after-free. Fix: reset the state_view
  (with handlers/timers) before closing. The 5.4 pin itself could not
  ship (vcpkg historical-port checkout fails on Windows) — reverted;
  baseline 5.5.1 accepted via ADR-025.

## M10 — Game framework: players, tilt, high scores (2026-08-27)

- Scope per 04 §M10: full GameState enum (TableSelect/Settings/Paused as
  placeholders until M18), 1–4 player rotation with per-player tb.state
  swap, ball count + extra balls, nudge → sim impulse + tilt bob with
  warnings/tilt, high scores (per-table top-10 under SDL_GetPrefPath,
  meta.default_scores seeding, initials entry), Start adds players.
- New `tb_game` (src/game): GameMachine (phase 3, driven by the solver
  hook between script dispatch and timers per 11 §1), HighScoreTable
  (crash-safe writes), InitialsEntry ring model, score formatter.
- Sim side: nudge half-sine envelopes (08 §7.1), damped tilt bob +
  leaky abuse accumulator with independent threshold re-arming
  (§7.2/§7.3), DangerThreshold sim events (framework-only, filtered
  from script dispatch), flippers/coils gates + ForceEjectAll /
  ResetDanger / LocksToTrough framework commands, framework-owned
  serving (M5 auto-serve loop steps aside when attached).
- state_hash now folds tilt-bob/abuse/envelope state (replayed state —
  nudges are inputs); flipper_tap + m2_bounce goldens regenerated per
  16 §2.4.4 (hash-scope change only, zero physics delta on nudge-free
  tapes).
- Spec reading documented in code: T10's five §2.5 conditions plus
  held/locked == 0 — §4.6 case B's 30 s watchdog only makes sense if
  captured balls hold BallInPlay open, so T10 cannot fire under a hold.
- Bug hunts this milestone: (1) serve_ball spawned without BallServed
  by design ("callers decide") — the framework's serve window never
  closed and re-commanded every 9 s; AddBall now serves via
  serve_ball_notified. (2) Phase-1 ordering: tick_event_n reset AFTER
  apply_script_actions wiped the serve's own event the same tick; the
  reset now precedes action application. (3) finish_bonus evaluated
  session_over BEFORE counting the finished player's ball — 4-player
  games played a 13th ball; counting split from the pointer advance,
  and the pointer only moves when the game continues so game_end fires
  with the last player current. (4) Extra balls unified on the host's
  PlayerScoreState (tb.award_extra_ball is the only award path; the
  machine's PlayerState kept a second, drifting copy).
- neon-drift ships its themed default_scores (15 §1.5: AXL 40M … KMH
  8M, exactly 10, V028-validated); test-lab declares none and starts
  empty by design (11 §7: no built-in ladder).
- Branch protection PUT retried once at M10 open: 404 again (no
  admin); journaled fallback per §3.2.
