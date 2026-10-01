# 16 — Testing & CI

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 03-process.md, 04-milestones.md, 05-engine-core.md, 08-physics.md,
09-table-format.md, 10-scripting.md, 12-audio.md, 14-authoring-guide.md,
15-launch-tables.md.

## 1. Philosophy

1. **Headless-first. The sim is the product.** Every physics behavior, rule,
   score, and table interaction must be testable in `tb_sim` + `tb_table` +
   `tb_game` with no GPU, no window, no audio device. Canon: `tb_sim` depends
   only on `tb_core`. Any test that needs a GPU is either a smoke test or a
   screenshot for human/LLM eyeballs — never a correctness gate.
2. **Renderer correctness is verified by screenshot review** (§5); renderer
   non-crash by smoke test. Pixel-exact GPU output is not gated in CI.
3. **Determinism is a tested invariant**, not an aspiration. Same binary +
   same seed + same input tape ⇒ identical simulation (PLAN.md §5.3).
   Cross-platform bit-exactness is a non-goal; goldens are per-OS (ADR-013,
   see 02-decisions.md).
4. **Every constant in a test is derived, not observed.** Unit tests assert
   values computed by hand (§2.1), never values copied from a first run of
   the code under test. Golden files are the only exception and have an
   explicit regeneration procedure (§2.4.4).

## 2. Test taxonomy and naming

Tests live in `/tests`, mirroring `/src` (canon §5.1), in the single gtest
binary `tb_tests`, except the tool-driven per-table tests (`validate_`,
`smoke_`, `bounds_`), which are CTest command tests. Naming: gtest suite and
case names are `snake_case`; the suite name starts with the taxonomy prefix.
CTest sees each test as `suite.case`, so CI filters by prefix regex. The
CTest command tests embed the pack directory slug verbatim, hyphens included
(`smoke_autoplay_neon-drift`), because the slug is the directory name.

| Taxonomy | Prefix | Example | Runs in |
|---|---|---|---|
| Unit (math, collision, schema, synth) | `unit_` | `unit_sweep.segment_interior_flat` | every job |
| Determinism | `det_` | `det_replay.twice_in_process_test_lab` | every job, **never retried** |
| Feel (FT scenarios) | `feel_` | `feel_scenarios.ft03_cradle_hold` | every job |
| Property / fuzz | `fuzz_sim_` | `fuzz_sim_invariants.random_1m_neon_drift` | perf job, weekly ASan |
| Loader corpus | `unit_loader` | `unit_loader.malformed_corpus` | every job |
| Script | `script_` | `script_api.golden_log` | every job |
| Pack validation | `validate_` | `validate_neon-drift` | every job, **from M15** (§2.2) |
| Autoplay smoke (300 s) | `smoke_` | `smoke_autoplay_neon-drift` | every job, **from M15** (§2.8) |
| Autoplay bounds (`--balls 3`) | `bounds_` | `bounds_autoplay_s2_neon-drift` | perf job only, **from M15** (§2.8) |
| Perf gates | `perf_` | `perf_tick.gate_synthetic` | perf job only (§2.9, §2.10) |

The per-milestone test lists in 04-milestones.md use shorthand PascalCase
`Suite.Case` labels (`Determinism.SameSeedSameHash`,
`HotPath.NoAllocationsInStep`, …). Those labels name **required coverage**,
not literal gtest identifiers: implement each under this section's
convention (04 global rule 7 defers naming here), choosing the prefix by
taxonomy — `Determinism.*` → `det_*`, FT feel scenarios →
`feel_scenarios.ftNN_*`, perf/latency gates → `perf_*`, fuzz → `fuzz_sim_*`,
everything else `unit_*` / `script_*` / `smoke_*` by content. The prefixes
are load-bearing: CI selects its retry, determinism, fuzz, and perf steps by
prefix regex (§3.2), so a mis-prefixed determinism test would silently land
in the retried main step — exactly the policy violation §6 forbids.

Fixture layout:

```
/tests
  /core /sim /table /audio /game /tools     # test sources, mirrors /src
  /fixtures
    /tables/          # minimal table fragments for unit tests
    /tapes/           # <slug>.replay.json input tapes (format §2.4.2)
    /schema/          # V-code fixture PACK DIRECTORIES (§2.2):
                      #   <vcode>_pass/ , <vcode>_fail[_<severity>]/
                      #   each holding whichever of table.json / rules.lua /
                      #   art.json / audio.json the rule reads
    /loader_corpus/   # 15 malformed-JSON files (§2.6.2)
    /patches/         # sfxr patch fixtures (§2.3)
    /lua/             # api_probe.lua and script fixtures
  /golden
    /determinism/{windows,linux,macos}/<slug>.hashes
    /script/api_log.txt                     # cross-platform
    /script/{windows,linux,macos}/test-lab.log
    /screenshots/{windows,linux,macos}/<slug>.png
    /synth/hashes.json                      # per-OS PCM hashes
  /third_party
    picosha2.h        # public-domain single-header SHA-256, vendored
  quarantine.txt
  lsan.supp
```

**Test data paths (normative — one mechanism, no alternatives).** Under CTest
the working directory is the preset's `binaryDir` (`build/<preset>/`), not the
source root, so **no test may depend on the process working directory**. The
test target carries
`target_compile_definitions(tb_tests PRIVATE TB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")`
and every repo-relative path is resolved through the single helper
`tb::test::data_path(rel)` (in `tests/support/data_path.h`), which joins
`TB_SOURCE_DIR` with `rel` and returns a `std::filesystem::path`. Everything
under `tests/fixtures/**`, `tests/golden/**`, `assets/**` and `tables/**` is
opened that way — starting with the first test written, M0's
`FontAssets.VendoredFontsPresentAndParse` (04-milestones.md M0), which reads
`assets/fonts/*.ttf` and `assets/fonts/SHA256SUMS`. `fopen("tests/…")`,
`../../tests/…`, and per-test `WORKING_DIRECTORY` properties are all
forbidden: they work from one build layout and break from the next. The CTest
command tests (`validate_`, `smoke_`, `bounds_`) get absolute paths from
`${CMAKE_SOURCE_DIR}` at registration time (snippet below) for the same
reason. SHA-256 inside tests comes from the vendored public-domain
`tests/third_party/picosha2.h` (recorded like any vendored asset) — never a
vcpkg crypto port and never a shell-out to `sha256sum`, which windows-latest
does not have.

CMake registration (`tests/CMakeLists.txt`):

```cmake
include(GoogleTest)
# Repo-relative test data: tests resolve paths through tb::test::data_path(),
# never the process working directory (see "Test data paths" above).
target_compile_definitions(tb_tests PRIVATE TB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Three discovery passes give per-class timeouts. Fallback if multiple passes
# on one target misbehave: a single pass with TIMEOUT 2400.
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "*-fuzz_sim_*:perf_*" PROPERTIES TIMEOUT 120)
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "fuzz_sim_*" PROPERTIES TIMEOUT 1800)
gtest_discover_tests(tb_tests DISCOVERY_MODE PRE_TEST
  TEST_FILTER "perf_*" PROPERTIES TIMEOUT 900)

# Tool readiness gate. tb_validate/tb_autoplay are usage-printing stubs from
# M0 (04-milestones.md M15 implements them), while /tables holds real packs
# from M5. Registering the per-table tool tests before M15 would turn every
# build-test job red for ten milestones, so registration is behind an option
# that is OFF until the M15 PR edits this line to ON (a one-line diff; no CI
# workflow change is needed, and no milestone before M15 can go red).
option(TB_TOOLS_READY "Register per-table tb_validate/tb_autoplay tests" OFF)

if(TB_TOOLS_READY)
  file(GLOB table_jsons CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tables/*/table.json")
  foreach(tj ${table_jsons})
    get_filename_component(slug_dir ${tj} DIRECTORY)
    get_filename_component(slug ${slug_dir} NAME)
    # Table directory is positional in both tools (09 §10, 14 §8.2 synopses).

    # 04-milestones.md global rule 4: tb_validate over all shipped tables on
    # every PR from M15. --strict promotes warnings to failures (09 §8).
    add_test(NAME validate_${slug} COMMAND tb_validate ${slug_dir} --strict)
    set_tests_properties(validate_${slug} PROPERTIES TIMEOUT 120)

    # Session-shape-independent bounds only (§2.8).
    add_test(NAME smoke_autoplay_${slug}
      COMMAND tb_autoplay ${slug_dir} --skill 1 --seconds 300
              --seed 7 --check-bounds)
    set_tests_properties(smoke_autoplay_${slug} PROPERTIES TIMEOUT 600)

    # Score/mode/shot bounds need full games; one test per measured skill
    # (§2.8). Skill 0 is registered too, because 14 §8.3 declares a
    # bound-able skill-0 shot-rate floor (`shots[<id>].rate` >= 0.02) and a
    # bound that matches no CI run is one §2.8 orders review to reject.
    # `--runs 20` is the authoring/iteration count (14 §5.1, §11 item 2) and
    # a deterministic prefix of 15 §0.7's binding `--runs 500` acceptance
    # sweep (§2.8). Run in the perf-gates job, not the 3-OS matrix.
    foreach(skill 0 1 2)
      add_test(NAME bounds_autoplay_s${skill}_${slug}
        COMMAND tb_autoplay ${slug_dir} --skill ${skill} --runs 20
                --seed 1 --balls 3 --check-bounds)
      set_tests_properties(bounds_autoplay_s${skill}_${slug}
        PROPERTIES TIMEOUT 900)
    endforeach()
  endforeach()
endif()

# Release gates (§2.10). Registered at M19; the frame-time gate is always
# registered and reports SKIPPED (exit 2) where no hardware GPU exists, so
# the gate can never be quietly deleted.
option(TB_RELEASE_GATES "Register the §2.10 release gates" OFF)  # ON at M19
if(TB_RELEASE_GATES)
  add_test(NAME perf_frame.gate_render_frame_time
    COMMAND ${CMAKE_COMMAND} -DTB_BIN=$<TARGET_FILE:tiltburst>
            -DTB_FRAMES=660 -DTB_WARMUP=60 -DTB_RUNS=3   # 600 timed frames
            -P ${CMAKE_SOURCE_DIR}/tools/perf/frame_gate.cmake)
  set_tests_properties(perf_frame.gate_render_frame_time
    PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 2)
endif()

# Flake quarantine (§6), committed empty at M0 (04-milestones.md M0 task 7):
# one `suite.case` per line; `#` comments and blank lines ignored. The read is
# at CONFIGURE time and must be guarded — an absent or empty file disables
# nothing and never fails configure. Application is at TEST time: with
# gtest_discover_tests(PRE_TEST) the gtest names exist only once CTest loads
# the test file, so a configure-time set_tests_properties() on one of them
# fails with "Can not find test to add properties to". The generated include
# below is appended to TEST_INCLUDE_FILES *after* the discovery calls above,
# so CTest processes it last, when the names exist; if(TEST ...) skips stale
# entries instead of erroring on them.
set(tb_quarantine "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt" tb_quarantine
       REGEX "^[^#[:space:]]")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt")  # re-read on edit
endif()
if(tb_quarantine)                      # empty list ⇒ skip the whole block
  list(LENGTH tb_quarantine tb_quarantine_n)
  if(tb_quarantine_n GREATER 5)
    message(WARNING "quarantine.txt holds ${tb_quarantine_n} entries; §6 caps"
                    " further quarantines at 5 — deflake the oldest first")
  endif()
  set(tb_q_file "${CMAKE_CURRENT_BINARY_DIR}/tb_quarantine.cmake")
  # TB_RUN_QUARANTINED=1 in the environment un-disables them, which is how
  # weekly-deep still runs them informationally (§3.2, §6).
  file(WRITE "${tb_q_file}"
    "# generated from tests/quarantine.txt — do not edit\n"
    "if(NOT \"\$ENV{TB_RUN_QUARANTINED}\" STREQUAL \"1\")\n")
  foreach(tb_q_test ${tb_quarantine})
    string(STRIP "${tb_q_test}" tb_q_test)
    file(APPEND "${tb_q_file}"
      "  if(TEST ${tb_q_test})\n"
      "    set_tests_properties(${tb_q_test} PROPERTIES DISABLED TRUE)\n"
      "  endif()\n")
  endforeach()
  file(APPEND "${tb_q_file}" "endif()\n")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${tb_q_file}")
  message(STATUS "Quarantined tests (16 §6): ${tb_quarantine}")
endif()
```

### 2.1 Unit tests — hand-computed collision math

Convention for all collision tests: the contact **normal points from the
surface toward the ball center**. Sweep functions are pure geometry with an
explicit horizon `t_max` (tests pass `t_max = 1.0 s`; sim CCD passes
`t_max = dt`). Assert with `EXPECT_NEAR`, absolute tolerance `1e-6` on
times/positions/normals (tighten to `1e-9` if 08-physics.md specifies
`double` state; it owns the numeric type).

Reference sweep algorithm (08-physics.md owns the shipped version; these
tests pin its observable results):

```
// Swept circle (center p0, radius r, velocity v) vs segment A→B on [0, t_max]
d  = normalize(B - A);  n = (-d.y, d.x)          // left normal
w  = p0 - A;  dist = dot(w, n)
if (dist < 0) { n = -n; dist = -dist }           // normal on ball's side
vn = dot(v, n)
if (vn < 0) {                                    // approaching the line
  t = (dist - r) / (-vn)
  s = dot((p0 + v*t - r*n) - A, d) / |B - A|
  if (0 <= t <= t_max && 0 <= s <= 1) -> interior hit (t, n)
}
// else endpoint caps: solve |p0 + v*t - E|^2 = r^2 for E in {A, B}:
//   a = dot(v,v); b = dot(p0-E, v); c = dot(p0-E, p0-E) - r^2
//   t = (-b - sqrt(b^2 - a*c)) / a              // smallest root >= 0
```

Five worked examples, ready to paste. Ball radius `r = 0.0135 m` throughout.

**Example 1 — `unit_sweep.segment_interior_flat`** (vertical drop onto a
horizontal wall). Given `p0 = (0.26, 0.50)`, `v = (0, −2.0) m/s`, segment
`A = (0.00, 0.10)`, `B = (0.52, 0.10)`.
Arithmetic: `n = (0, 1)`, `dist = 0.50 − 0.10 = 0.40`, `vn = −2.0`,
`t = (0.40 − 0.0135) / 2.0 = 0.3865 / 2.0`.
Expect: `t = 0.19325 s`, normal `(0, 1)`, contact point `(0.26, 0.10)`
(segment parameter `s = 0.26/0.52 = 0.5`), ball center at TOI
`(0.26, 0.1135)`.

**Example 2 — `unit_sweep.segment_interior_45deg`**. Given
`p0 = (0.30, 0.10)`, `v = (−1.0, 1.0) m/s`, segment `A = (0.10, 0.10)`,
`B = (0.30, 0.30)`.
Arithmetic: `d = (0.70710678, 0.70710678)`; `w = (0.20, 0)`;
`dist = |0.20·0.70710678 − 0| = 0.14142136`; ball-side normal
`n = (0.70710678, −0.70710678)`;
`vn = −1·0.70710678 + 1·(−0.70710678) = −1.41421356`;
`t = (0.14142136 − 0.0135) / 1.41421356 = 0.1 − 0.0135/√2`.
Expect: `t = 0.09045406 s`, normal `(0.70710678, −0.70710678)`, ball center
at TOI `(0.20954594, 0.19045406)`, contact point `(0.20, 0.20)` (`s = 0.5`).

**Example 3 — `unit_sweep.segment_endpoint_cap`** (path misses the interior,
clips an endpoint). Given `p0 = (0.10, 0.30)`, `v = (0, −3.0) m/s`, segment
`A = (0.30, 0.10)`, `B = (0.11, 0.10)`; the swept path passes 0.01 m to the
left of endpoint `B`, closer than `r`.
Arithmetic (cap quadratic on `E = B`): `e = p0 − B = (−0.01, 0.20)`;
`a = 9`; `b = 0.20·(−3) = −0.6`;
`c = 0.0001 + 0.04 − 0.00018225 = 0.03991775`;
`b² − a·c = 0.36 − 0.35925975 = 0.00074025`; `√ = 0.02720754`;
`t = (0.6 − 0.02720754)/9`.
Expect: `t = 0.06364361 s`, ball center at TOI `(0.10, 0.10906918)`, normal
`(−0.74074074, 0.67179101)` (= `(center − B)/r`; also assert `|n| = 1`
within `1e-6`).

**Example 4 — `unit_sweep.arc_inside_hit`** (ball inside a circular guide,
effective radius `R − r`). Given arc center `C = (0.26, 0.52)`, `R = 0.10 m`;
`p0 = (0.26, 0.48)`, `v = (0, 1.5) m/s`.
Arithmetic: `e = p0 − C = (0, −0.04)`; `a = 2.25`; `b = −0.04·1.5 = −0.06`;
`c = 0.0016 − 0.0865² = −0.00588225` (negative: started inside the effective
circle ⇒ take the `+√` root);
`b² − a·c = 0.0036 + 0.01323506 = 0.01683506`; `√ = 0.12975`;
`t = (0.06 + 0.12975)/2.25`.
Expect: `t = 0.08433333 s`, ball center at TOI `(0.26, 0.6065)`, normal
`(0, −1)` (inward), contact point on the arc `(0.26, 0.62)`.

**Example 5 — `unit_integrate.slope_gravity_1s`** (closed form for
semi-implicit Euler, canon slope). Effective gravity
`a = 9.81·sin(6.5°) = 9.81·0.11320321 = 1.11052353 m/s²` along −y. From rest
at `p0 = (0.26, 0.80)`, `dt = 0.001`, `N = 1000` ticks of
`v += a·dt; p += v·dt`:
`Δy = −a·dt²·N(N+1)/2 = −1.11052353e−6 · 500500 = −0.55581703 m`.
Expect: `v_y = −1.11052353 m/s` (tol `1e-5`), `y = 0.24418297 m` (tol
`1e-4`, float accumulation). If 08-physics.md specifies a different
integrator, derive the matching closed form and update the expected
constants in the same PR — never loosen tolerances to keep old constants.

Additional required unit coverage (same rigor; derivation in a comment above
each test): restitution reflection, flipper angular-to-linear velocity
transfer at a given contact radius, broadphase cell coverage of a swept
ball, PCG32 known-answer sequence (seed 42, first 5 outputs from the PCG
reference implementation), FNV-1a 64 known-answer
(`fnv1a64("tb") = 0x08c82e07b56aadf3`).

### 2.2 Schema / loader tests

Every validation rule code (V-code) defined in 09-table-format.md §8 must
have **both** a passing and a failing fixture. A fixture is a **pack
directory**, not a single file:

```
tests/fixtures/schema/<vcode>_pass/
tests/fixtures/schema/<vcode>_fail/            # single-severity codes
tests/fixtures/schema/<vcode>_fail_error/      # codes with two severities
tests/fixtures/schema/<vcode>_fail_warning/
```

Each directory contains whichever of `table.json`, `rules.lua`, `art.json`,
`audio.json` the rule actually reads — V001 needs only `table.json`; V034
needs `table.json` + `art.json`; V037 needs `table.json` + `audio.json`;
V032/V033 need `table.json` + `rules.lua`. Packs are minimal: the smallest
valid pack that isolates the rule (start from `tables/test-lab/`, 09 §7).

Three properties, all enforced structurally rather than by convention:

1. **Coverage.** `unit_schema.vcode_coverage` iterates
   `tb::table::all_validation_codes()` — which returns each code **with its
   declared severity or severities** — and fails if any (code, severity)
   pair lacks a fail pack, or any code lacks a pass pack. Codes enter that
   list as their milestone implements them, so the test scopes itself.
2. **Exactness.** `unit_schema.vcode_fires_<code>` runs the fail pack and
   asserts the diagnostic set contains **exactly** that code at exactly the
   09 §8 severity (other codes present ⇒ the fixture is not isolated ⇒
   fail). The pass pack must produce **zero** diagnostics.
3. **Severity behavior.** The fail pack aborts loading **only for
   error-severity codes**. 17 of the catalog's codes are warning-severity
   — V002 (rules.lua ids), V005 (recommended band), V006, V007, V012, V014,
   V020, V025, V026, V027, V031, V032, V033, V035, V036, V038, V039 — and
   their fail packs must **load successfully** while emitting the warning.
   The assertion for a warning code is therefore: loader/validator returns
   success, the warning is present, `tb_validate` exits 0, and
   `tb_validate --strict` exits 2 (09 §10). Asserting "load fails" on a
   warning code is the single most likely way to write this suite wrong.

Codes with two severities (V002 error for `art_ref`/`prefab` vs warning for
rules.lua ids; V005 error outside the hard range vs warning outside the
recommended band; V007 warning for a stray region vs error when the flipper
area is unreachable) get one fail pack per severity, named with the
`_fail_error` / `_fail_warning` suffixes above.

**V000–V031** run through the in-game loader API (the same code path the
game uses), so their tests assert both the diagnostics and the load
outcome. **V032–V039** are `tb_validate`-only authoring-loop checks that the
loader deliberately skips (09 §8), so they are exercised through the
validator entry point that `tb_validate` wraps
(`tb::table::validate_pack(dir, opts)`), never through `load_table` — a
V032–V039 test that expects the loader to speak is testing a behavior 09
forbids. These packs and tests land at M15 with the tooling; before M15 the
codes are absent from `all_validation_codes()` and the coverage test passes
trivially.

### 2.3 Synth tests (sfxr)

`unit_synth.patch_render_deterministic`: render the fixture patch
`tests/fixtures/patches/laser.json` twice in-process through the full
12-audio.md §5 pipeline (44 100 Hz generation, linear resample to 48 kHz);
the two final 48 kHz mono `float` PCM buffers must be bit-identical
(compare raw IEEE-754 bit patterns, not `==` on floats).
`unit_synth.patch_render_nonsilent`: peak `|sample|` ≥ 0.1 and RMS ≥ 0.01
(full scale = 1.0) — a synth that renders silence passes no other test.
`unit_synth.patch_render_golden`: FNV-1a 64 hash over the raw bit patterns
of the final float PCM equals the per-OS entry in
`tests/golden/synth/hashes.json` (float math differs per compiler; ADR-013
same-OS policy, regeneration per §2.4.4).

### 2.4 Determinism tests

#### 2.4.1 State hash

`tb::sim::state_hash()` (in `tb_sim`) is FNV-1a 64
(`h = 0xcbf29ce484222325`; per byte `h ^= b; h *= 0x100000001b3`) over a
canonical serialization: each ball in ball-id order — position, velocity,
spin, layer as raw IEEE-754 bit patterns; then each element's mechanical
state in `table.json` declaration order (enums as `int32`, floats as bit
patterns); then the PCG32 stream state (both sim-owned streams,
08-physics.md §2.2 rule 4); then tilt state and score; then, last, the eight
bytes of the **event-sequence accumulator** defined next.

**The event-sequence accumulator.** 05-engine-core.md §6.3, 08-physics.md
§2.2 rule 7, 10-scripting.md §2.2 and 11-game-framework.md §1 all state that
event emission order is part of the deterministic record and that this
suite compares those sequences, not just final scores. Ball state alone
cannot carry that: an event reordered, duplicated, or dropped without moving
a ball is invisible to a position-only hash, and only `test-lab` — whose
`script_testlab.golden_run` (§2.7) logs every event — would ever catch it.
So the sequence is hashed, for every table:

`tb::sim::EventSeqHash` is one `uint64_t` owned by `tb_sim`, initialized to
the FNV-1a offset basis `0xcbf29ce484222325` when the sim is constructed and
**never reset** — not per tick, not per game. It is a *rolling* hash, which
is what makes one `state_hash()` sample every 1,000 ticks (§2.4.3) cover the
events of all 1,000 ticks between samples. Each event absorbs exactly three
fields, at the moment it is dispatched, in this field order:

| Field | Type | Value |
|---|---|---|
| `tick` | `uint64` | the sim tick dispatching the event |
| `type` | `uint16` | the `SimEventType` enum value (05-engine-core.md §8.1; physics types 08, script types 10) |
| `id` | `uint16` | the event's identifying index: `SimEvent::element` for sim events (`0xFFFF` = none); for framework events the 0-based player index the event concerns, `0xFFFF` where none does |

absorbed as raw bytes in that field order, under the same bit-pattern rule
as the rest of this section (native byte order is fine — goldens are per-OS,
ADR-013). Nothing else is absorbed: payload floats already reach the hash
through ball and element state, and hashing them here would turn every
payload tweak into a golden regeneration.

Dispatch order is the tick pipeline's order and is fully pinned by
08-physics.md §2.1 step 7 — phase 2 sim events in generation order, then
phase 3 framework events in FSM emission order, then phase 4's `timer_tick`
events in ascending `timer_id`. The framework lives in `tb_game`, above
`tb_sim` in the layering (canon §5.1), so it **pushes** into the accumulator
through a `tb_sim` sink (`absorb_event(uint64 tick, uint16 type, uint16 id)`)
as it emits, exactly like tilt state and score already reach the hash. There
is no reverse dependency and no second hash to reconcile.

#### 2.4.2 Replay tape format (owned by this doc)

```json
// tests/fixtures/tapes/test-lab.replay.json
{
  "version": 1,
  "table_slug": "test-lab",
  "seed": 424242,
  "inputs": [
    [0, 0],        // [tick, buttons_bitmask]; strictly increasing ticks
    [1500, 16],    // plunger pull starts at tick 1500
    [2100, 0],     // plunger released
    [5000, 1],     // left flipper down
    [5180, 0]
  ]
}
```

Each entry's bitmask applies from its tick (inclusive) until the next entry.
Tape bits have their own fixed numbering — one numbering, never renumbered;
05-engine-core.md maps its logical input actions onto these bits (the
bit → action table in 05 §13.1 is normative):

| Bit | Tape name | Bit | Tape name |
|---|---|---|---|
| 0 | flipper_left | 5 | launch |
| 1 | flipper_right | 6 | nudge_left |
| 2 | flipper_upper_left | 7 | nudge_right |
| 3 | flipper_upper_right | 8 | nudge_forward |
| 4 | plunger_pull | 9 | start |

Bit 5 `launch` is an alias of bit 4 — v1 has a single plunger/launch
action; if bits 4 and 5 change together, one edge is emitted (05 §13.1).
Bits 10–15 are reserved and must be 0 — tapes are gameplay-only. Replay v1
is digital-only; if 05-engine-core.md later adds analog plunger position,
that requires `"version": 2`, never a silent extension.

Two replay formats exist by design, with distinct owners. The **runtime**
record/replay file — binary `.tbreplay`, magic `TBRP`, 96-byte header,
per-edge records, written by `--record` — is owned by 05-engine-core.md
§13. This JSON tape (`<slug>.replay.json`) is the **test fixture** format,
owned by this doc: level-based, human/LLM-writable, diffable in review. The
tape bits above reach the sim through the 05 §13.1 mapping onto the 05 §9.1
action indices: a tape is expanded into the §13 edge stream before
injection, one edge per changed bit in ascending bit order. Converting a
`.tbreplay` recording down to a fixture tape collapses per-edge records into
bitmask-change entries and is therefore lossy wherever the recording holds
sub-millisecond taps (05 §13); `tb_autoplay --replay` accepts either format,
dispatching on file extension. Exactly two extensions exist — `.tbreplay`
for runtime recordings, `.replay.json` for test tapes — and no third format
or spelling is permitted anywhere in the plan.

#### 2.4.3 The tests

`det_replay.twice_in_process_<slug>` — for `test-lab` and every shipped
table: run 100,000 ticks twice in-process with the same seed and the same
**procedural tape**, recording `state_hash()` every 1,000 ticks; the two
hash sequences must be identical. The procedural tape needs no fixtures:

```
rng = PCG32(seed = 0x54425F44, stream = fnv1a64(table_slug))
mask = 0; tick = 0
while tick < 100000:
  tick += 50 + (rng.next() % 351)          // gap uniform in [50, 400]
  mask ^= 1 << pick(rng, {0, 1, 4, 6, 7, 8})  // §2.4.2 tape bits:
                                             // flippers, plunger, 3 nudges
  emit (tick, mask)
```

`det_golden.same_os_<slug>` — run the checked-in tape
`tests/fixtures/tapes/<slug>.replay.json` for 100,000 ticks and compare the
hash-per-1,000-ticks sequence against
`tests/golden/determinism/<os>/<slug>.hashes`. `GTEST_SKIP()` with a logged
message if the tape or golden does not exist yet (they land at M15).

`det_replay.flipper_tape_hash_stable` — from **M4**; this is
04-milestones.md M4's `Determinism.FlipperReplayHashStable` under §2's
naming convention. It replays a committed **flipper-heavy** tape from
`tests/fixtures/tapes/` (§2.4.2 format) twice in-process and compares the
hash-per-1,000-ticks sequences. It is not redundant with its neighbours and
none of the three replaces another: `det_replay.twice_in_process_<slug>`
drives a procedural tape, `det_feel.twice_in_process_ft03` (§2.5) drives a
code rig with predicate-triggered input and no file at all, and this one is
the only test that exercises the committed-tape path — tape parse plus the
05 §13.1 expansion into the edge stream. Before M5 there is no loader, so
the harness builds its scene in code (like the §2.9 M2 synthetic scene); from
M5 the tape's `table_slug` names a real pack.

Because §2.4.1's hash now carries the rolling event-sequence accumulator,
comparing hash sequences **is** comparing event sequences: a run that
reproduces every ball position but emits `multiball_end` and `ball_end` in
the other order, or drops a `timer_tick`, diverges at the next sample. That
is what makes the promise of 05 §6.3 / 08 §2.2 rule 7 / 10 §2.2 / 11 §1 true
for `test-lab` and every shipped table, rather than only for the one table
whose golden run logs events (§2.7).

#### 2.4.4 Golden policy (ADR-013)

- Goldens are **per-OS** (`windows`, `linux`, `macos` directories); CI
  compares same-OS only. Cross-OS comparison is forbidden — it fails by
  design and proves nothing (canon: cross-platform bit-exactness is a
  non-goal).
- Golden file format, one line per sample:

```
# tiltburst determinism golden v1
# table: test-lab  tape: test-lab.replay.json  seed: 424242
1000 3f9c2d1a5e8b4c07
2000 91be77a0cc41d2e8
```

- Regeneration: `tb_autoplay tables/<slug> --replay <tape>
  --record-golden <path>`. A PR that intentionally changes sim behavior must
  regenerate goldens for all three OSes: every CI matrix job records fresh
  goldens into `build/goldens/<os>/` and uploads them as artifacts (§3
  YAML); the author downloads the three artifacts, commits them under
  `tests/golden/determinism/`, and notes the regeneration and reason in
  `docs/JOURNAL.md`. Regenerating a golden without a JOURNAL entry is a
  review-blocking offense. The same procedure covers synth, script, and
  screenshot goldens.

### 2.5 Feel tests (FT-01 … FT-10)

The feel scenarios are defined — geometry, inputs, assertions, numeric
tolerances — in 08-physics.md §5.6 (the rig) and §5.7 (the scenarios).
**08-physics.md §5.6 is normative for the harness**; this section only binds
it into the suite. FT-01…FT-08 are tagged M4, FT-09/FT-10 M8.

The rig is **built in test code**: no `table.json`, no table fixture, no
replay tape, no file of any kind under `tests/fixtures/` or
`tests/data/replays/`. The harness constructs the 08 §5.6 collider set
(play area 0.52 × 1.04 m, slope 6.5°, the two flippers, the two inlane
guides, the two posts, the closed border wall, the outhole) directly against
the `tb_sim` API, seeds the sim RNG with `0x54425354`, and adds the 08 §5.6
"M8 additions" (`magnet_m`, `ramp_r`) **only** for FT-09/FT-10, so the M4
scenarios see the M4 rig unchanged.

Input is **state-triggered, never tick-scheduled**: the harness evaluates
each scenario's predicate after every tick and injects the button state for
the next tick, exactly as 08 §5.6 specifies ("press when `ball.y ≤ 0.26`" →
evaluate `ball.y ≤ 0.26` at the end of tick *n*, inject on tick *n+1*).
Time-based instructions in 08 §5.7 (`release left at t = 2.0 s`) are ticks
counted from the scenario's own zero, not from a tape. This is why a
§2.4.2 `[tick, bitmask]` tape cannot express these scenarios: a tape pins
the press to an absolute tick, so every constant tuned in 08 §5 (stroke
time, restitution, slope) silently moves the ball past the intended trigger
point and the scenario degrades into a different test. The predicate form is
immune to that and stays deterministic.

Each scenario is one gtest case `feel_scenarios.ftNN_<name>` (names from 08
§5.7: `ft01_dead_bounce`, `ft02_live_catch`, `ft03_cradle_hold`,
`ft04_backhand`, `ft05_post_pass`, `ft06_tap_pass`, `ft07_tip_shot_power`,
`ft08_cradle_escape_slap`, `ft09_magnet_catch_throw`,
`ft10_ramp_make_rollback`), asserting ball state at the ticks and within the
tolerances 08 §5.7 gives — its bands are the contract and are never widened
here. Shared helpers (`cradled(flipper)`, `CRADLE_SETUP`) are implemented
once from 08 §5.6's definitions. Feel tests run headless in every CI job.

Because the rig is code with a fixed seed and predicate-driven input, an FT
scenario is reproducible with no fixture to keep in sync — and the rig is
itself hash-stable: `det_feel.twice_in_process_ft03` re-runs FT-03 twice
in-process and compares `state_hash()` every 1,000 ticks. That test proves
*the rig* is deterministic. It is **not** 04-milestones.md M4's
`Determinism.FlipperReplayHashStable`, which has a different purpose: that
one replays a committed flipper-heavy `.replay.json` tape and so also covers
the tape parse and the 05 §13.1 expansion into the edge stream, a path the
code rig never touches. It lands at M4 as
`det_replay.flipper_tape_hash_stable` (§2.4.3) and does need a tape file;
the feel suite still never gets one. If 08-physics.md adds FT-11+, tests
follow it; a scenario without an automated test is a spec violation.

### 2.6 Property / fuzz tests

#### 2.6.1 Sim invariants under random input

`fuzz_sim_invariants.random_1m_<slug>` — for each shipped table, run
1,000,000 ticks (1,000 s simulated) with random input: each tick, with
probability 0.005, toggle a uniformly chosen bit in {0..9}. Fixed seed
`0xF0221` per-PR; the weekly job overrides via env `TB_FUZZ_SEED` (set to
the CI run id) and logs the seed first so any failure is reproducible.
Checked every tick:

- **I1 bounds:** every ball satisfies `−0.05 ≤ x ≤ width + 0.05`,
  `−0.15 ≤ y ≤ height + 0.05` (trough region is below `y = 0`),
  `0 ≤ z ≤ 0.15`.
- **I2 finiteness:** no NaN/Inf in any snapshot float.
- **I3 speed clamp:** post-tick `|v| ≤ 12.0 + 1e−6 m/s` (canon §5.3).
- **I4 events sane:** event ticks non-decreasing; every ball id referenced
  by an event exists; cumulative `drain` count ≤ cumulative launch count;
  the `SimEvent` ring buffer never overflows.

`fuzz_sim_invariants.passive_energy_<slug>` — same run with all actuators
disabled (flippers, slingshots, pop bumpers, kickers, magnets off; test hook
`SimTestApi::set_actuators_enabled(false)`): total mechanical energy
`E = Σ balls [½·0.08·|v|² + 0.08·9.81·sin(slope)·y + 0.08·9.81·z]` (plus
element-stored terms 08-physics.md defines, e.g. gate springs) may increase
by at most `1e−6 J` in any single tick and must not increase over any
1,000-tick window by more than `1e−5 J`. Restitution < 1 guarantees decay;
growth means an integration or impulse bug.

Fuzz tests run in the Release perf job per PR (≈ 10–60 s wall per table)
and under ASan weekly (§4.3).

#### 2.6.2 Loader fuzz corpus

`unit_loader.malformed_corpus` iterates every file in
`tests/fixtures/loader_corpus/`; for each, the loader must return a
structured error (a V-code, or the parse-error code 09-table-format.md
assigns) within 5 s — never crash, hang, or leak (leak-checked under ASan,
§4.3). The 15 checked-in cases:

| File | Content |
|---|---|
| `01_empty.json` | zero-byte file |
| `02_truncated.json` | valid prefix, unclosed brace |
| `03_root_array.json` | valid JSON, root is an array not an object |
| `04_unknown_type.json` | element `"type": "warp_portal"` |
| `05_duplicate_id.json` | two elements with id `"w1"` |
| `06_missing_required.json` | flipper with no pivot |
| `07_wrong_type.json` | `"radius": "big"` (string where number) |
| `08_nan_literal.json` | bare `NaN` token in a coordinate |
| `09_deep_nesting.json` | 1,000-deep nested arrays |
| `10_huge_number.json` | coordinate `1e308` |
| `11_negative_radius.json` | post radius `−0.01` |
| `12_invalid_utf8.json` | UTF-8 BOM followed by `0xFF 0xFE` mid-string |
| `13_dangling_reference.json` | light referencing a nonexistent element id |
| `14_degenerate_wall.json` | wall segment with identical endpoints |
| `15_resource_bomb.json` | 100,000 walls (element-count limit must trip) |

### 2.7 Script tests

`script_api.golden_log` — cross-platform golden. A mock event injector feeds
`tests/fixtures/lua/api_probe.lua` a fixed sequence of synthetic events with
exact literal payloads (no physics involved); the script exercises every
`tb.*` function in PLAN.md §5.7. The harness logs each delivered event and
each action call as `tick=<n> event=<name> ...` /
`tick=<n> call=<fn>(<args>)` (floats printed `%.6f`) and compares to
`tests/golden/script/api_log.txt`. All values are exact inputs, so this
golden is OS-independent.

`script_testlab.golden_run` — integrated, per-OS golden (physics floats flow
into payloads). Run `test-lab` with its `rules.lua` and the checked-in tape
for 60,000 ticks; log `(tick, event_name, element_id, score_after)` and
compare to `tests/golden/script/<os>/test-lab.log`.

`script_sandbox.*` — assert `io`, `os`, `require`, `load` are absent,
`math.random` is replaced, and the instruction-count watchdog fires on
`while true do end` (10-scripting.md owns the limit value).

### 2.8 Autoplay smoke and bounds gating

Two CI shapes, because 14-authoring-guide.md §8.2's two session shapes are
disjoint: a `--seconds 300` session never plays a full game (instant
respawn, no game-over), so `score.*` and `modes.*` are undefined in it,
while a `--balls 3` sweep never runs a continuous session and its
`coverage.share` is not comparable. A single run therefore cannot gate all
of `meta.autoplay_bounds`. Both shapes are registered in §2's CMake snippet
under `TB_TOOLS_READY`, i.e. **registration lands at M15** with the tools
(04-milestones.md M15); the table directory is positional and every flag
spelling is 14 §8.2's normative synopsis — this doc never invents a flag.

**(a) Smoke run — session-shape-independent metrics.** For every directory
under `/tables`, CTest runs

```
tb_autoplay tables/<slug> --skill 1 --seconds 300 --seed 7 --check-bounds
```

Pass requires exit code 0, which tb_autoplay grants only when: zero log
lines at `error` severity, `stuck_balls` = 0 (stuck detection spec in 14
§8.2), and every **declared bound measured at skill 1 in a `--seconds`
session** is inside its range. The metrics gated here are 14 §8.3's five
session-shape-independent rows — `stuck_balls`, `drains.center`,
`drains.outlane_share`, `coverage.share`, `ball_time_s.p50` (the same five
15-launch-tables.md §0.7 mirrors) — plus `script_errors` = 0, a pass
condition regardless of bounds. They are independent of session shape, so a
300 s run measures them honestly. The list is *session-shape*-exhaustive,
not skill-exhaustive: a session-independent metric whose declared skill is
not 1 (14 §8.3 reads `tilts` at skill 2) is not evaluated by this run and is
gated by the (b) run at its own skill — which is why (b) covers every skill
14 §8.3 uses.

**(b) Bounds job — score / mode / shot metrics.** For every table and every
skill 14 §8.3 measures a bound at, CTest runs

```
tb_autoplay tables/<slug> --skill {0|1|2} --runs 20 --seed 1 --balls 3 \
            --check-bounds
```

registered as `bounds_autoplay_s<skill>_<slug>` and executed in the
`perf-gates` job (§3.2), never in the 3-OS matrix — full games are the
expensive shape and one Linux Release run of them is enough. This gates
`shots[<id>].rate` at skill 2 (≥ 0.10 for every labeled shot, ≥ 0.40 for
difficulty 1–2 shots) **and at skill 0** (the ≥ 0.02 floor),
`modes.started_per_game` and `modes.multiball_reach_share` (skill 1),
`score.p50` and `modes.wizard_reach_share` (skill 2), plus any `tilts` bound
(declared at skill 2; 14 §8.3 reads it in either shape). **All three skills
run**: 14 §8.3 declares a bound-able skill-0 shot-rate floor, and this
section rules below that a bound matching no CI run must be rejected in
review — so omitting skill 0 would make a conformant table simultaneously
required and rejectable.
Budget: 20 runs × ~105 s of simulated 3-ball game ≈ 35 min of sim time per
test, ~10–30 s of wall time in Release; 18 tests (6 packs × 3 skills) is
3–9 min of wall time, inside the `perf-gates` job's 45-minute timeout with
room to spare. If they ever stop fitting, drop the sweep at any skill for
which a table declares no bound — never raise the per-test `TIMEOUT` past
900 s to hide a hang.

**Where the numbers come from.** Bounds are calibrated per table in
15-launch-tables.md §0.7 and mirrored, machine-readable, into that pack's
`table.json` `meta.autoplay_bounds` (09-table-format.md §2 owns the block
and the metric-path grammar, including `shots[<id>].rate`; V029 validates
it). **Every declared bound records the skill it is measured at**, and
`--check-bounds` evaluates a bound only when the run's `--skill` equals that
skill and the run's session shape can measure the metric; a bound that
matches no CI run is a V029-clean but useless declaration and review must
reject it. The score *spread* ratio (`score.p90 ÷ score.p10`) of 15 §0.7 is
a review-only target and is deliberately **not** mirrored into bounds —
p10/p90 over 20 runs is too noisy to gate.

**Why `--runs 20` is honest.** Two counts exist and both are binding, at
different moments: `--runs 20` is the authoring/iteration count (14 §5.1's
EGS check and §11 item 2 — fast feedback while tuning), and `--runs 500` is
the per-table **acceptance** suite (15 §0.7). CI runs the 20. 14 §8.2 seeds
run *i* with `S + i`, so `--seed 1 --runs 20` is exactly the first 20 runs
of 15 §0.7's `--seed 1 --runs 500` sweep — the CI sample is a deterministic
prefix of the acceptance sample, reproducible locally byte for byte. A
bound must hold on **both** the 500-run sweep and the 20-run prefix; if the
prefix falls outside while the sweep is inside, widen the declared bound
(with a JOURNAL.md note) rather than weaken the table or the gate.

**When a bounds gate fails, which fix list.** A red `smoke_autoplay_`/
`bounds_autoplay_` test is a table-tuning problem, not a CI problem, and
there is exactly one first move. For a **shipped** table, apply
15-launch-tables.md §0.7's ordered five-step fix list first — outlane gap,
ball-save duration, slingshot `kick_speed`, `slope`, shot-mouth posts — it is
the first resort precisely because it is written against the standard bottom
(15 §0.4) all five shipped tables instantiate. 14-authoring-guide.md §8.4's
per-metric tuning matrix is the general authoring tool: it applies to any
table and covers everything those five steps do not reach (coverage, shot
rates, EGS, mode and wizard reach, the 15 % rule), including a shipped table
whose missed metric is not one of the five. The two never compete for the
same first move. Widening a *declared bound* is the only CI-side remedy and
is legitimate only in the case above — 500-run sweep inside, 20-run prefix
outside — never as a substitute for tuning.

### 2.9 Performance gates

One harness, one protocol, one pair of limits — what it *loads* grows with
the milestone ladder, because a gate cannot depend on machinery that does
not exist yet (no table loader before M5, no Lua before M9). 08-physics.md
§9's CI gate cites this section's harness; there is no second harness and no
recorded "perf-stress" replay.

This doc owns test ids (§2), so the two ids below — **`perf_tick.gate_synthetic`
and `perf_tick.gate_tables`** — are the only tick-gate names in the plan.
Any other spelling (`gate_all_tables`, `gate_frame_time`) is stale and is
corrected to these, never the reverse.

**Perf gate inventory (complete).** 04-milestones.md global rule 4 defers
*every* gate id to this document, so every id in the plan is one of these
seven. All are selected by the `^perf_` prefix (§2), all are Release-only,
and all run in the `perf-gates` job
(`ctest --preset release -R '^perf_' -E '^$' -j1` — the `-E '^$'` is
load-bearing, see §3.2's filter rule); each row carries its own protocol and
thresholds in the section that owns it.

| Gate id | From | Defined in | Gates |
|---|---|---|---|
| `perf_tick.gate_synthetic` | M2 | §2.9 | mean < 100 µs, p99 < 200 µs on the 88-collider code-built scene |
| `perf_tick.gate_tables` | M5 (rules.lua from M9) | §2.9 | the same two limits, per shipped pack |
| `perf_latency.input_to_tick_p999` | M4 | §2.9 | input→latch p99.9 < 4 ms over ≥ 10,000 scripted press edges |
| `perf_particles.two_thousand_live_at_60fps` | M13 | §2.9 | particle CPU cost < 1.5 ms/frame with 2,000 live particles |
| `perf_startup.cold_boot_to_attract` | M19 | §2.10(a) | cold start < 1,500 ms |
| `perf_frame.gate_render_frame_time` | M19 | §2.10(b) | p99 < 8.0 ms, max < 16.7 ms; SKIPPED on software rasterizers |
| `perf_load.table_under_2s` | M19 | §2.10(c) | full pack load < 2,000 ms |

A document citing "the perf gates" means exactly these seven; a document
citing "the release gates" means the last three.

| From | Gate | Scene | Adds |
|---|---|---|---|
| **M2** | `perf_tick.gate_synthetic` | programmatically built collider scene — **no `table.json`, no Lua, no fixture file** | proves the harness itself early |
| **M5** | `perf_tick.gate_tables` | `test-lab` + every shipped table pack, loaded through `tb_table` | real geometry, prefab expansion |
| **M9** | same `perf_tick.gate_tables` | the tables' `rules.lua` is loaded and its handlers run in the timed window | script cost inside the tick |

`perf_tick.gate_synthetic` never retires — it is the noise-free reference
point that separates "the sim got slower" from "this table got heavier".

**The synthetic scene** (M2, built in test code, fully specified so it is
reproducible): play area 0.52 × 1.04 m, slope 6.5°, default materials.
Colliders — the closed play-area border (4 wood segments); an 8 × 10 lattice
of rubber posts of radius 0.008 m at `x = 0.05 + 0.06·i` (i = 0…7, so
0.05…0.47) and `y = 0.12 + 0.08·j` (j = 0…9, so 0.12…0.84), giving
post-surface gaps of 0.060 − 0.016 = 0.044 m horizontally and
0.080 − 0.016 = 0.064 m vertically, both > the 0.027 m ball diameter so
balls circulate instead of wedging; and 4 steel arcs (lower half-circles,
180°→360°) centered at (0.16, 0.98) and (0.36, 0.98) with radii 0.05 and
0.08 m — lowest arc point `0.98 − 0.08 = 0.90 m`, clearing the top post row
(`0.84 + 0.008 = 0.848 m`) by 0.052 m. **88 colliders** total
(4 + 80 + 4). Four balls spawn at (0.10, 1.00), (0.20, 1.00), (0.30, 1.00),
(0.40, 1.00), each velocity component drawn uniformly from [−3, +3] m/s by
`PCG32(seed = 99)`. The scene has no outhole and a closed border, so all
four balls stay live for the whole timed window.

Compiled into `tb_tests`, meaningful only in Release (`GTEST_SKIP()` unless
`NDEBUG` is defined). Runs only in the `perf-gates` CI job (Linux, Release).
Harness, per scene (the synthetic one, then per table from M5):

```
for run in 1..3:
  build scene (M2: synthetic; M5+: load table; M9+: also load rules.lua)
  SimTestApi::spawn_balls(4, seed = 99)      // 4 balls in play, canon default
  tick 5,000 warmup (untimed)
  tick 60,000 timed: steady_clock around each full sim tick
       (input latch + integration + CCD + script events + snapshot publish)
  record mean and p99 (sorted durations, index ceil(0.99·N)−1)
gate on the MEDIAN of the 3 means and the MEDIAN of the 3 p99s
```

Gate (build **fails** on violation), applied to the synthetic scene and to
every shipped table — the busiest table is covered automatically; the test
also logs which table has the highest post-prefab-expansion collider count:

| Metric | CI limit | Reference-hardware budget (08-physics.md §9) |
|---|---|---|
| median-of-3 mean tick time | < 100 µs | ≤ 50 µs |
| median-of-3 p99 tick time | < 200 µs | ≤ 200 µs |

The CI mean limit is 08-physics.md §9's gate: 2× the reference budget,
deliberate margin for noisy shared runners. The 50 µs mean budget itself is
verified manually on reference hardware at M19 — it is not a CI number.

Variance handling: median-of-3 absorbs shared-runner noise; the limits carry
≥ 10× headroom over the expected single-digit-µs tick; the perf step runs
`ctest -j1` so nothing competes for the core. If the gate fails twice
consecutively on a PR that touches none of `src/sim`, `src/table`,
`src/game`, treat it as runner pathology: note in JOURNAL.md and re-run.
Thresholds change only via an ADR in 02-decisions.md. The harness writes
`build/release/perf_report.json` (per-scene mean/p99 per run); CI uploads it
as an artifact.

**Input latency gate** — `perf_latency.input_to_tick_p999`, from **M4**,
same job. Drives ≥ 10,000 scripted press edges through the real late-latch
path of 05-engine-core.md §9 with synthetic `InputEdge` timestamps, records
edge-timestamp → tick-consumption age per edge, and gates on the single
binding statement used everywhere in this plan (01-product.md R2.1 and §9,
04-milestones.md M4, 05-engine-core.md Done-when): **p99.9 < 4 ms over
≥ 10,000 scripted press edges**. The sample count is always stated in the
same sentence as the percentile — a p99.9 over 500 samples is five events
and means nothing. The measured p99.9 goes into `perf_report.json`.

**Particle gate** — `perf_particles.two_thousand_live_at_60fps`, from
**M13**, same job. 04-milestones.md M13 describes the harness (and the F12
GPU-capture protocol that sits beside it); this section owns the id and the
numbers, so global rule 4's "gate ids are owned by 16-testing-ci.md" holds
for it like the other six. It is a **headless gtest** — no GPU, no window —
that drives the CPU particle system of 06-rendering.md §13 with **2,000
simultaneously live particles** for **600 update steps at `dt` = 16.67 ms**
(10.0 s of 60 Hz frames) and gates the per-frame *particle update + instance
build* on:

| Metric | CI limit | Source |
|---|---|---|
| median-of-3 mean per-frame particle CPU cost | **< 1.5 ms** | 06-rendering.md §17.1 CPU-encode budget |
| allocations during the timed window | **0** | 01-product.md R3.2, pool sized at build |

**1.5 ms is 9.0 % of the 16.67 ms 60 Hz frame period, and the period is not
the budget**: gating on 16.67 ms would be ~11× looser and would pass no
matter how slow the system got. Discipline is the tick gates' above,
unchanged — `GTEST_SKIP()` unless `NDEBUG` (Release-only), three runs, gate
on the median of the three run means, `-j1`, per-run numbers into
`perf_report.json` — and the `perf_` prefix puts it in the existing
`perf-gates` step with no workflow edit at M13. The **GPU** half of R3.2 is
deliberately not gated here (CI runners are software-rasterized, §2.10b):
M13 evidences it with the F12 capture `docs/audit/m13/multiball.png`, and
from M19 `perf_frame.gate_render_frame_time` covers frame time on any runner
with a hardware GPU.

**Which 06-rendering.md §17.1 budgets a CI job can enforce.** §17.1 heads its
table "binding; gated in 16-testing-ci.md perf jobs", and this section is
where that claim is cashed out — for the runner-independent rows only. The
budgets are all binding on the product; they are not all *gateable on a
hosted runner*, because those runners are software-rasterized (§2.10b):

| §17.1 row | Where it is enforced |
|---|---|
| CPU encode / particle CPU cost ≤ 1.5 ms | **gated on any runner** — `perf_particles.two_thousand_live_at_60fps`, headless, no GPU |
| Playfield ≤ 200 and backglass ≤ 40 draw calls, ≤ 6000 SDF instances, ≤ 8192 particle pool | **countable on any runner** — a software rasterizer issues the same draw calls and builds the same instances; they are CPU-side counts read from the F1 overlay (06 §17.2) and reviewed with the M13/M19 capture evidence. No `perf_` gate asserts them today, and none may be coined outside §2.9's seven-id inventory without an ADR. |
| Playfield GPU time ≤ 4.0 ms, VRAM ≤ 256 MB | **hardware-only** — not measurable on llvmpipe/lavapipe/WARP; evidence is the M13 F12 capture and the M19 hardware run of `perf_frame.gate_render_frame_time`, which reports CTest SKIPPED on every hosted runner (§2.10b) |

So "exceeding a budget is a CI failure, not a note" holds for row 1 and, once
a count is in front of a reviewer, row 2. For row 3 the honest statement is
that CI cannot see it: the failure surfaces at M19 on hardware. Never widen a
§17.1 threshold, or a §2.10(b) limit, because a software rasterizer missed
it — the correct hosted-runner result there is SKIPPED.

### 2.10 Release gates (M19/M20)

04-milestones.md M19 gates on a cold-start budget, a frame-time gate, and a
table-load budget; M20's audit cites them. There are **exactly three release
gates**, and these are their canonical ids (this doc owns test ids, §2):
`perf_startup.cold_boot_to_attract` (a), `perf_frame.gate_render_frame_time`
(b), `perf_load.table_under_2s` (c). `gate_cold_start`, `gate_frame_time`
and `gate_table_load` are stale spellings and are corrected to these three.
This section defines all three with numbers and harnesses, plus the
determinism-soak procedure (d) that replaces 01-product.md §9's undefined
"100 consecutive green runs per platform" — (d) is a procedure, not a fourth
gate. (a) and (c) are gtest cases compiled into `tb_tests` at M19; (b) is a
CTest command test registered behind `TB_RELEASE_GATES` (§2's snippet),
turned ON in the M19 PR. All of them run in `perf-gates` from M19 on.

**(a) `perf_startup.cold_boot_to_attract`** — gtest, Release only. Cold
start = process launch + bootstrap + reaching Attract. Measured in two
parts, both with existing binaries and flags:

| Part | How measured | CI limit | Profile A budget |
|---|---|---|---|
| process launch (exec + dynamic link + static init) | spawn `tiltburst --version`, wait for exit, wall clock, **median of 5** (the binary path reaches the test as the compile definition `TB_TILTBURST_EXE=$<TARGET_FILE:tiltburst>`) | < 400 ms | ≤ 250 ms |
| headless bootstrap → Attract | in-process: 05-engine-core.md §1 steps 2–13 with `--headless --table test-lab`, `tb::now_ns()` from before step 2 to the first tick whose snapshot reports the FSM in `Attract` (11-game-framework.md §2.1), **median of 3** | < 1,100 ms | ≤ 750 ms |
| **sum — the gate** | | **< 1,500 ms** | ≤ 1,000 ms |

The full GPU path (device creation, window claim, audio device) is not
headless-measurable; on Profile A it must still reach Attract within the
**2,000 ms** splash budget of 01-product.md §4.1, verified manually at M19
by subtracting the two `[+seconds]` log timestamps of 05 §12 (step-5
startup line, Attract transition line) and adding the launch measurement.
That manual number is fallback-eligible per 01 §3 (PROVISIONAL-PASS +
JOURNAL note) exactly like the 50 µs tick budget. M19 factors 05 §1 steps
2–13 into `tb::app::boot(const CliOptions&)` (in `tb_game`) so the test can
call it and `main()` becomes a thin wrapper; if that factoring is refused in
review, the fallback is the log-timestamp procedure above run as a scripted
measurement, recorded in JOURNAL.md — the budget itself never moves.

**(b) `perf_frame.gate_render_frame_time`** — CTest command test driving
`tools/perf/frame_gate.cmake` (a CMake script, like `tools/package/`; no new
executable target). The driver runs, three times,

```
tiltburst --render-smoke --frames 660 --screenshot-dir <tmp>
```

and parses the machine-readable summary line §5 requires
(`render_smoke: frames=… warmup=… mean_ms=… p99_ms=… max_ms=… backend=…
software=0|1`), discarding the first 60 frames as warmup:

| Metric | Limit (hardware GPU) | Rationale |
|---|---|---|
| median-of-3 p99 frame time | < 8.0 ms | half the 16.67 ms budget of a 60 Hz refresh (R1), leaving headroom for present + compositor |
| median-of-3 max frame time | < 16.7 ms | one refresh period at 60 Hz; matches 01 R1.1's < 17.5 ms max frame gap |

**Skip path (documented, never a deletion).** From M19 the test is
registered on every platform and every runner — never conditioned on GPU
presence — with `SKIP_RETURN_CODE 2`. The driver exits 2 — CTest shows
**SKIPPED**, not passed — when `tiltburst --render-smoke` itself exits 2 (no
usable GPU backend) **or** when the summary line reports `software=1`
(`llvmpipe`/`lavapipe`, `SwiftShader`, D3D12 `WARP`, "Microsoft Basic Render
Driver"): a software rasterizer at 1080 × 1920 cannot meet a hardware frame
budget, and gating on it would force the threshold to be widened into
meaninglessness. GitHub's hosted runners are software-rasterized, so in
practice this gate SKIPs in CI and is *run* on the M19 developer machine or
the cabinet; the skip is visible in every `perf-gates` summary, the M19 PR
must paste the non-skipped local run, and M20 records it PROVISIONAL-PASS
per 01 §3 if only the cabinet was unavailable. Deleting or `if()`-ing out
the registration is a review-blocking offense — a skipped gate is a
reminder, a missing gate is a lie.

**(c) `perf_load.table_under_2s`** — gtest, Release only, one case per pack
directory under `/tables` (`test-lab` included). Measures the full pack load exactly as
05 §1 step 11 does — `table.json` parse + prefab expansion + validation +
`build_sim` + `rules.lua` chunk load + `art.json` + `audio.json` parse —
median of 3, cold process each run is not required (the budget is a user-
visible latency, not an I/O benchmark):

| Metric | CI limit | Note |
|---|---|---|
| median-of-3 full pack load, per table | **< 2,000 ms** | **this section (§2.10c) owns the 2,000 ms pack-load budget** |
| median-of-3 `table.json` + `build_sim` only | < 1,000 ms | the stricter path behind M5's "F5 reloads in under 1 s" |

Both numbers are defined here and nowhere else: 04-milestones.md M19 gates on
this id and explicitly disclaims owning the budget, and 09-table-format.md
defines no load budget at all — "table load < 2 s per 09-table-format.md" is
the phantom citation 04's own pitfall list names. Changing either limit is an
edit to this table plus an ADR in 02-decisions.md.

The gate measures whichever pack files exist at the current milestone
(`table.json` from M5, `rules.lua` from M9, `audio.json` from M11,
`art.json` from M13); from M13 it always measures all four. Per-table
milliseconds go into `perf_report.json`.

**(d) Determinism soak — the replacement for "100 consecutive green runs".**
01-product.md §9's success metric is this exact procedure, runnable by one
command per OS:

```sh
ctest --preset <release|windows-release> -R '^det_' -E '^$' \
      --repeat until-fail:100 --output-junit junit-det-soak.xml
```

`--repeat until-fail:100` runs **every** `det_` test up to 100 times and
stops at the first failure, so a green run is literally 100 consecutive
green runs of each determinism test. In CI it is the `det-soak` job of §3.2:
`workflow_dispatch` with input `det_soak: true`, matrix over the three OSes,
`timeout-minutes: 120` (expected ≈ 15–20 min: ~14 `det_` tests — 7
`det_replay` (6 procedural + the M4 flipper tape, §2.4.3), 6 `det_golden`,
`det_feel` — averaging under 1 s of Release sim time each, × 100 iterations). Pass = all three jobs green with zero
failures; evidence = the three run URLs and the junit artifacts, pasted into
the M20 audit row for the determinism metric. It is **not** a required check
(it is manual, and 100× is far too slow per PR); the per-PR determinism step
of §3.2 remains the merge gate. Run it once during M19 and once during M20
before tagging; a failure is a live determinism bug and blocks the release
(§6: `det_` tests never retry, never quarantine).

## 3. CI workflows

Three CI files, complete below: the composite action (§3.1) and
`build-test.yml` (§3.2) are committed at M0; `release.yml` (§3.3) is
committed at M19. Coexistence with `zai-code-review.yml`: the review
workflow triggers on `pull_request_target` with concurrency group
`zai-review-<PR#>`; build-test triggers on `pull_request`/`push` with group
`ci-<ref>` — different events, different namespaces, zero interference. The
review workflow is never a required check (it self-skips on drafts and
missing secrets); the checks below are.

**Required checks for merge on `main`** — exactly six contexts:

```
format
build-test (ubuntu-latest)
build-test (windows-latest)
build-test (macos-latest)
asan
perf-gates
```

Those six strings are **deterministic, not descriptive**: §3.2 gives the
matrix job an explicit `name: build-test (${{ matrix.os }})` template, so
GitHub names the three check runs exactly as written above (a bare
`name: build-test` on a matrix job produces three check runs that all report
as `build-test` and cannot be required individually — that is why the
template is mandatory). The three single-job names come from their own
`name:` keys.

03-process.md owns the branch-protection **setup** (the
`gh api -X PUT repos/{owner}/{repo}/branches/main/protection` call, with
`required_pull_request_reviews: null` — no human approval is ever required —
and `enforce_admins: false`), executed as an M0 task. Before running it,
copy the context strings **verbatim** from `gh pr checks <M0-PR>` on the M0
PR rather than from this prose; if GitHub ever renders a name differently
than the list above, `gh pr checks` wins and this section is corrected in
the same PR. If protection cannot be configured at all (no admin rights on
the repo), proceed: the per-PR CI-green checklist of 03-process.md §4 is
then the enforcing mechanism, and the substitution is recorded in
JOURNAL.md (03-process.md fallback matrix) — a milestone never blocks on it.

`weekly-deep` and `det-soak` are **not** required (both are manual/scheduled
only), and `release.yml` runs only on tags. Do not add `paths:` filters — a
docs-only PR must still produce these checks or branch protection holds it
forever.

### 3.1 `.github/actions/tb-setup/action.yml` (composite, deduplicates job setup)

```yaml
name: Tiltburst build setup
description: Linux SDL3 deps, vcpkg GHA binary cache env, pinned vcpkg.
runs:
  using: composite
  steps:
    - name: Install Linux system dependencies (SDL3 build deps + lavapipe)
      if: runner.os == 'Linux'
      shell: bash
      run: |
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
          ninja-build pkg-config build-essential jq \
          libasound2-dev libpulse-dev libaudio-dev libjack-dev libsndio-dev \
          libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
          libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
          libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
          libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev \
          libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev \
          libvulkan-dev mesa-vulkan-drivers

    - name: Install Ninja (macOS)
      if: runner.os == 'macOS'
      shell: bash
      run: brew install ninja

    - name: Export GitHub Actions cache env for vcpkg
      uses: actions/github-script@v7
      with:
        script: |
          core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
          core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

    - name: Set up vcpkg pinned to the manifest baseline
      shell: bash
      run: |
        BASELINE="$(jq -r '."builtin-baseline"' vcpkg.json)"
        git clone https://github.com/microsoft/vcpkg "${RUNNER_TEMP}/vcpkg"
        git -C "${RUNNER_TEMP}/vcpkg" checkout "${BASELINE}"
        if [ "${RUNNER_OS}" = "Windows" ]; then
          "${RUNNER_TEMP}/vcpkg/bootstrap-vcpkg.bat" -disableMetrics
        else
          "${RUNNER_TEMP}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
        fi
        echo "VCPKG_ROOT=${RUNNER_TEMP}/vcpkg" >> "${GITHUB_ENV}"
```

### 3.2 `.github/workflows/build-test.yml`

```yaml
name: Build & Test

on:
  push:
    branches: [main]
  pull_request:
  schedule:
    - cron: "0 3 * * 1" # weekly deep run: ASan fuzz, Mondays 03:00 UTC
  workflow_dispatch:
    inputs:
      det_soak:
        description: "Also run the 100-iteration determinism soak (§2.10d)"
        type: boolean
        default: false

permissions:
  contents: read

# Distinct namespace from zai-code-review.yml (zai-review-<PR#>).
concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

env:
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"

jobs:
  format:
    name: format
    if: github.event_name != 'schedule'
    runs-on: ubuntu-latest
    timeout-minutes: 10
    steps:
      - uses: actions/checkout@v4
      - name: clang-format check (version pinned to 18, 03-process.md §1.4)
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends clang-format-18
          find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 \
            | xargs -0 -r clang-format-18 --dry-run --Werror

  build-test:
    # Explicit template: without it a matrix job's three check runs are all
    # named "build-test" and cannot be required individually (§3).
    name: build-test (${{ matrix.os }})
    if: github.event_name != 'schedule'
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        include:
          - os: ubuntu-latest
            configure_preset: release
            build_preset: release
            test_preset: release
          - os: macos-latest
            configure_preset: release
            build_preset: release
            test_preset: release
          - os: windows-latest
            configure_preset: windows
            build_preset: windows-release
            test_preset: windows-release
    runs-on: ${{ matrix.os }}
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup

      - name: Configure
        run: cmake --preset ${{ matrix.configure_preset }}

      - name: Build
        run: cmake --build --preset ${{ matrix.build_preset }}

      - name: Test (flake-retried; excludes det/fuzz/perf/bounds)
        shell: bash
        run: |
          ctest --preset ${{ matrix.test_preset }} \
            -E '^(det_|fuzz_sim_|perf_|bounds_)' \
            --repeat until-pass:2 --output-junit junit-main.xml

      - name: Determinism tests (never retried)
        shell: bash
        run: |
          ctest --preset ${{ matrix.test_preset }} -R '^det_' -E '^$' \
            --no-tests=ignore --output-junit junit-det.xml

      - name: Record determinism goldens (artifact for intentional sim changes)
        if: always()
        shell: bash
        run: |
          case "${RUNNER_OS}" in
            Linux) OSDIR=linux ;; Windows) OSDIR=windows ;; macOS) OSDIR=macos ;;
          esac
          AUTOPLAY=$(find build -type f \( -name tb_autoplay -o -name tb_autoplay.exe \) | head -n1)
          if [ -n "${AUTOPLAY}" ] && [ -d tables ]; then
            mkdir -p "build/goldens/${OSDIR}"
            for tj in tables/*/table.json; do
              slug=$(basename "$(dirname "${tj}")")
              tape="tests/fixtures/tapes/${slug}.replay.json"
              [ -f "${tape}" ] && "${AUTOPLAY}" "tables/${slug}" \
                --replay "${tape}" \
                --record-golden "build/goldens/${OSDIR}/${slug}.hashes" || true
            done
          fi

      - name: Renderer smoke (exit 2 = logged skip, see 16-testing-ci.md §5)
        shell: bash
        run: |
          BIN=$(find build -type f \( -name tiltburst -o -name tiltburst.exe \) | head -n1)
          if [ -z "${BIN}" ]; then echo "::warning::tiltburst not built yet"; exit 0; fi
          set +e
          "${BIN}" --render-smoke --frames 120 --screenshot-dir build/smoke
          CODE=$?
          set -e
          if [ "${CODE}" = "0" ]; then echo "renderer smoke OK";
          elif [ "${CODE}" = "2" ]; then echo "::warning::renderer smoke skipped: no usable GPU backend";
          else echo "renderer smoke crashed (exit ${CODE})"; exit "${CODE}"; fi

      - name: Upload test logs, goldens, screenshots
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: test-output-${{ matrix.os }}
          if-no-files-found: ignore
          path: |
            junit-*.xml
            build/**/junit-*.xml
            build/*/Testing/Temporary/*.log
            build/goldens/**
            build/smoke/**
            build/screens/**

  asan:
    name: asan
    if: github.event_name != 'schedule'
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup
      - name: Configure and build (ASan+UBSan)
        run: |
          cmake --preset asan
          cmake --build --preset asan
      - name: Test under ASan+UBSan (excludes det/fuzz/perf)
        run: ctest --preset asan -E '^(det_|fuzz_sim_|perf_)' --repeat until-pass:2
      - name: Determinism under ASan (never retried)
        run: ctest --preset asan -R '^det_' -E '^$' --no-tests=ignore

  perf-gates:
    name: perf-gates
    if: github.event_name != 'schedule'
    runs-on: ubuntu-latest
    timeout-minutes: 45
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup
      - name: Configure and build Release
        run: |
          cmake --preset release
          cmake --build --preset release
      # Every step below pairs -R with -E '^$': the release test preset
      # excludes ^(fuzz_sim_|perf_|bounds_), a CLI -R replaces only the
      # preset's include, and without the -E the selection is empty (§3.2
      # implementor notes).
      - name: Fuzz invariants (1M ticks per table, fixed seed)
        run: ctest --preset release -R '^fuzz_sim_' -E '^$' --no-tests=ignore
      - name: Autoplay bounds (--balls 3 sweeps, §2.8b; none before M15)
        run: ctest --preset release -R '^bounds_' -E '^$' --no-tests=ignore
      - name: Perf gates (Release, serial; §2.9 + §2.10)
        run: ctest --preset release -R '^perf_' -E '^$' -j1 --no-tests=ignore
      - name: Upload perf report
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: perf-report
          if-no-files-found: ignore
          path: build/release/perf_report.json

  weekly-deep:
    name: weekly-deep
    if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
    runs-on: ubuntu-latest
    timeout-minutes: 45
    env:
      TB_FUZZ_SEED: ${{ github.run_id }}
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup
      - name: Configure and build (ASan+UBSan)
        run: |
          cmake --preset asan
          cmake --build --preset asan
      - name: Randomized fuzz under ASan (seed = run id, logged by the test)
        run: ctest --preset asan -R '^fuzz_sim_' -E '^$' --no-tests=ignore
      - name: Quarantined tests (informational, may fail)
        continue-on-error: true
        shell: bash
        env:
          TB_RUN_QUARANTINED: "1" # un-disables them for this step only (§2, §6)
        run: |
          if [ -s tests/quarantine.txt ]; then
            RE=$(paste -sd'|' tests/quarantine.txt)
            ctest --preset asan -R "^(${RE})$" -E '^$' --no-tests=ignore
          fi

  # Determinism soak (§2.10d): the runnable form of 01-product.md §9's
  # "100 consecutive green runs per platform". Manual only, never required.
  det-soak:
    name: det-soak (${{ matrix.os }})
    if: github.event_name == 'workflow_dispatch' && inputs.det_soak
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        include:
          - os: ubuntu-latest
            configure_preset: release
            build_preset: release
            test_preset: release
          - os: macos-latest
            configure_preset: release
            build_preset: release
            test_preset: release
          - os: windows-latest
            configure_preset: windows
            build_preset: windows-release
            test_preset: windows-release
    runs-on: ${{ matrix.os }}
    timeout-minutes: 120
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup
      - name: Configure and build Release
        run: |
          cmake --preset ${{ matrix.configure_preset }}
          cmake --build --preset ${{ matrix.build_preset }}
      - name: 100 consecutive determinism runs (stops at first failure)
        shell: bash
        run: |
          ctest --preset ${{ matrix.test_preset }} -R '^det_' -E '^$' \
            --repeat until-fail:100 --no-tests=ignore \
            --output-junit junit-det-soak.xml
      - name: Upload soak junit
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: det-soak-${{ matrix.os }}
          if-no-files-found: ignore
          path: junit-det-soak.xml
```

Implementor notes:

- First Windows run compiles SDL3 and friends (~20–30 min); the `x-gha`
  vcpkg binary cache makes later runs ~2 min. Cache entries written by a
  failed job persist, so a cold-cache timeout heals on re-run. The 45-minute
  timeout accounts for the cold case.
- M0 ships `unit_scaffold.sanity` (trivial passing test) so the main test
  step never hits "no tests found" (`noTestsAction: error` in the presets);
  det/fuzz/perf/bounds steps use `--no-tests=ignore` until their milestones
  land.
- No workflow edit is needed at M15 or M19: the per-table tool tests and the
  release gates appear the moment `TB_TOOLS_READY` / `TB_RELEASE_GATES` flip
  to ON in `tests/CMakeLists.txt` (§2), and the existing `-R` steps pick
  them up. Before those flips the steps legitimately match nothing.
- The golden-record step is best-effort by design (`|| true`): `tb_autoplay`
  is a usage-printing stub until M15, and no `.replay.json` tapes exist
  before then, so the step writes no goldens and must not fail the job.
- A `det_soak` dispatch also triggers `weekly-deep` (its `if` accepts any
  `workflow_dispatch`). That is harmless — weekly-deep is informational —
  but expect two extra ASan jobs on the run page.
- `--repeat until-pass:2` implements the flake policy (§6) for everything
  except determinism: a `det_` test passing on retry is itself a determinism
  bug, so those steps never retry.
- **Filter rule (get this wrong and a required check goes green forever).** A
  command-line `-R` overrides only a preset's `filter.include.name`; the
  preset's `filter.exclude.name` still applies. Every test preset inherits
  `test-base`'s `exclude: ^(fuzz_sim_|perf_|bounds_)` (§4.1), so
  `ctest --preset release -R '^perf_'` intersects "perf only" with "no perf"
  and selects **zero** tests — which `--no-tests=ignore` then reports as a
  pass. Therefore every step that pairs `-R` with a preset carrying an
  exclude also passes an explicit non-matching `-E '^$'`, which replaces the
  preset exclude. The main test step needs no `-R`, so it instead states the
  full exclusion union in its own `-E`.
- **Prove the selection is non-empty at M2.** The first perf gate
  (`perf_tick.gate_synthetic`) lands at M2; run
  `ctest --preset release -R '^perf_' -E '^$' -N` and confirm it lists that
  test (`Total Tests: 1` or more). Dropping the `-E` prints `Total Tests: 0`,
  which is exactly the silent-green failure above. Repeat the `-N` check for
  `-R '^fuzz_sim_'` and `-R '^bounds_'` in the PR that lands each category's
  first test (`bounds_` at M15); before that an empty list is legitimate and
  `--no-tests=ignore` is why the steps stay green.
- The `format` job needs no vcpkg or compiler — it fails in under a minute
  on an unformatted file, before the matrix finishes configuring.

### 3.3 `.github/workflows/release.yml` (committed at M19)

Tag-push packaging per 04-milestones.md M19: build on all three OSes,
package (Windows `.zip`, macOS `.app`-in-`.dmg`, Linux `.tar.gz`), smoke
test the packaged build (unpack, `tiltburst --version`, `tb_validate` on a
bundled table), stamp the version from the git tag, attach everything to a
draft GitHub release. `tools/package/package.cmake` (M19 scope; 04 owns the
archive contents) dispatches on platform and must write exactly
`build/pkg/<artifact>`.

```yaml
name: Release

on:
  push:
    tags: ["v*"]

permissions:
  contents: write # create the draft release and attach artifacts

concurrency:
  group: release-${{ github.ref }}

env:
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"

jobs:
  package:
    name: package
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        include:
          - os: ubuntu-latest
            configure_preset: release
            build_preset: release
            artifact: tiltburst-linux-x64.tar.gz
          - os: macos-latest
            configure_preset: release
            build_preset: release
            artifact: tiltburst-macos.dmg
          - os: windows-latest
            configure_preset: windows
            build_preset: windows-release
            artifact: tiltburst-windows-x64.zip
    runs-on: ${{ matrix.os }}
    timeout-minutes: 60
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/tb-setup

      - name: Configure and build (version stamped from the tag)
        shell: bash
        run: |
          cmake --preset ${{ matrix.configure_preset }} \
            -DTB_RELEASE_TAG="${GITHUB_REF_NAME}"
          cmake --build --preset ${{ matrix.build_preset }}

      - name: Package (tools/package/, contents per 04-milestones.md M19)
        shell: bash
        run: |
          cmake -DTB_PRESET=${{ matrix.configure_preset }} \
                -DTB_ARTIFACT=${{ matrix.artifact }} \
                -P tools/package/package.cmake
          test -f "build/pkg/${{ matrix.artifact }}"

      - name: Smoke test the packaged build
        shell: bash
        run: |
          SMOKE="${RUNNER_TEMP}/tbsmoke"; mkdir -p "${SMOKE}"
          ART="build/pkg/${{ matrix.artifact }}"
          case "${RUNNER_OS}" in
            Linux)   tar -xzf "${ART}" -C "${SMOKE}" ;;
            Windows) unzip -q "${ART}" -d "${SMOKE}" ;;
            macOS)   hdiutil attach "${ART}" -mountpoint /Volumes/tbsmoke
                     cp -R /Volumes/tbsmoke/. "${SMOKE}/"
                     hdiutil detach /Volumes/tbsmoke ;;
          esac
          BIN=$(find "${SMOKE}" -type f \( -name tiltburst -o -name tiltburst.exe \) | head -n1)
          VAL=$(find "${SMOKE}" -type f \( -name tb_validate -o -name tb_validate.exe \) | head -n1)
          TABLE=$(find "${SMOKE}" -type f -name table.json | head -n1)
          test -n "${BIN}" && test -n "${VAL}" && test -n "${TABLE}"
          "${BIN}" --version | grep -F "${GITHUB_REF_NAME#v}"
          "${VAL}" "$(dirname "${TABLE}")"

      - name: Upload workflow artifact
        uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.artifact }}
          path: build/pkg/${{ matrix.artifact }}

      - name: Attach to the draft GitHub release
        shell: bash
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release create "${GITHUB_REF_NAME}" --draft \
            --title "Tiltburst ${GITHUB_REF_NAME}" \
            --notes "See CHANGELOG.md" \
            || true # a sibling matrix job already created it
          gh release upload "${GITHUB_REF_NAME}" \
            "build/pkg/${{ matrix.artifact }}" --clobber
```

Notes: the release stays a **draft**; M20 publishes it after the
Definition-of-Done audit (`gh release edit v1.0.0 --draft=false`).
`-DTB_RELEASE_TAG` overrides the `src/core/version.h` version string, and
the smoke step's `grep` proves the stamp took. The `grep -F` on the version
and the `tb_validate` run implement 04 M19's packaged-build smoke test;
macOS bundles stay unsigned (Gatekeeper note per 04 M19). `release.yml` is
not a required check — it never runs on PRs.

## 4. Local dev loop

### 4.1 `CMakePresets.json` (committed at M0)

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 28, "patch": 0 },
  "configurePresets": [
    {
      "name": "base", "hidden": true,
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON" }
    },
    {
      "name": "debug", "inherits": "base", "generator": "Ninja",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" },
      "condition": { "type": "notEquals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "release", "inherits": "base", "generator": "Ninja",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" },
      "condition": { "type": "notEquals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "asan", "inherits": "base", "generator": "Ninja",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address,undefined"
      },
      "condition": { "type": "notEquals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "windows", "inherits": "base",
      "generator": "Visual Studio 17 2022",
      "architecture": { "value": "x64", "strategy": "set" },
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    }
  ],
  "buildPresets": [
    { "name": "debug", "configurePreset": "debug" },
    { "name": "release", "configurePreset": "release" },
    { "name": "asan", "configurePreset": "asan" },
    { "name": "windows-debug", "configurePreset": "windows", "configuration": "Debug" },
    { "name": "windows-release", "configurePreset": "windows", "configuration": "Release" }
  ],
  "testPresets": [
    {
      "name": "test-base", "hidden": true,
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error", "stopOnFailure": false },
      "filter": { "exclude": { "name": "^(fuzz_sim_|perf_|bounds_)" } }
    },
    { "name": "debug", "inherits": "test-base", "configurePreset": "debug" },
    { "name": "release", "inherits": "test-base", "configurePreset": "release" },
    {
      "name": "asan", "inherits": "test-base", "configurePreset": "asan",
      "environment": {
        "ASAN_OPTIONS": "abort_on_error=1:detect_leaks=1",
        "LSAN_OPTIONS": "suppressions=${sourceDir}/tests/lsan.supp",
        "UBSAN_OPTIONS": "print_stacktrace=1"
      }
    },
    { "name": "windows-debug", "inherits": "test-base", "configurePreset": "windows", "configuration": "Debug" },
    { "name": "windows-release", "inherits": "test-base", "configurePreset": "windows", "configuration": "Release" }
  ]
}
```

Local prerequisite: clone vcpkg anywhere and export `VCPKG_ROOT`. The
`test-base` filter excludes fuzz/perf/bounds from every test preset by
default. A CLI `-R` replaces only the preset's *include*, so selecting an
excluded category takes **both** flags — `-R '^perf_' -E '^$'` — and a bare
`-R '^perf_'` silently selects nothing; a CLI `-E` is what replaces the
preset exclude. That pairing is exactly how CI selects them (§3.2). To
exercise the tool-driven per-table
tests locally before M15 flips the default, configure with
`-DTB_TOOLS_READY=ON` (they will fail against the stubs — that is the
point of the option).

### 4.2 Everyday invocations

```sh
cmake --preset debug && cmake --build --preset debug   # build
ctest --preset debug                                    # default suite
# -E '^$' is mandatory with -R: it replaces the preset's exclude filter,
# which otherwise cancels the selection outright (§3.2 filter rule).
ctest --preset debug   -R '^det_'      -E '^$'          # just determinism
ctest --preset release -R '^fuzz_sim_' -E '^$'          # fuzz locally
ctest --preset release -R '^perf_'     -E '^$' -j1      # perf gates (§2.9)
ctest --preset release -R '^perf_'     -E '^$' -N       # what would run
ctest --preset release -R '^bounds_'   -E '^$' --no-tests=ignore  # bounds
ctest --preset release -R '^det_' -E '^$' --repeat until-fail:100 # soak §2.10d
# One test, straight from the binary (fastest iteration):
./build/debug/tb_tests --gtest_filter='unit_sweep.segment_endpoint_cap'
# Windows: build\windows\Debug\tb_tests.exe --gtest_filter=...
```

### 4.3 Sanitizer policy (decided)

- The `asan` preset (ASan + UBSan, RelWithDebInfo so the 1M-tick fuzz stays
  fast; `TB_CHECK` invariant macros from 05-engine-core.md stay active under
  `NDEBUG`) runs **on every PR** on Linux via the `asan` job. Since one PR =
  one milestone (PLAN.md §2), this satisfies "per-milestone" with margin.
- The **weekly** `weekly-deep` job additionally runs the 1M-tick fuzz under
  ASan with a randomized seed (`TB_FUZZ_SEED` = run id, logged first).
  Reproduce locally:
  `TB_FUZZ_SEED=<id> ./build/asan/tb_tests --gtest_filter='fuzz_sim_*'`.
- MSVC ASan and macOS sanitizers are not in CI; run them locally when
  chasing a platform-specific memory bug.

## 5. Renderer smoke and screenshots in CI

`tiltburst --render-smoke --frames 120 --screenshot-dir <dir>`: initialize
SDL video with `SDL_VIDEODRIVER=dummy` if no display server is present,
create the SDL3 GPU device **without a window**, render `--frames` frames of
the `test-lab` table into an offscreen target, write the final frame as PNG
(stb image write), print per-frame timing, exit 0. Exit codes: `0` success,
`2` no usable GPU backend (CI logs a warning and continues — see the YAML
step), anything else is a crash and fails the job.

The **last** line of stdout is a machine-readable summary, parsed by the
§2.10(b) frame-time gate (frame times in ms, excluding the first 60 warmup
frames; `backend` from `SDL_GetGPUDeviceDriver`, `software=1` when the
adapter/driver name matches `llvmpipe`, `lavapipe`, `SwiftShader`, `WARP`,
or "Microsoft Basic Render Driver"):

```
render_smoke: frames=660 warmup=60 mean_ms=1.842 p99_ms=4.107 max_ms=6.550 backend=vulkan software=1
```

Runner reality: Windows runners
reach D3D12 (WARP), macOS 14+ runners reach Metal, Linux runners reach
Vulkan via lavapipe (`mesa-vulkan-drivers` is installed by `tb-setup`);
expect exit 2 rarely, but the skip path must exist and must log.

Screenshot review flow (from M15, when `tb_screenshot` exists): the matrix
job renders every shipped table to `build/screens/<os>/<slug>.png` (guarded
like the golden-record step) and uploads them in the `test-output-*`
artifacts. Reference images live in
`tests/golden/screenshots/{windows,linux,macos}/<slug>.png`, regenerated
deliberately per §2.4.4. They are compared by **human/LLM eyeball in PR
review, never pixel-gated** — GPU output varies by driver and that is fine;
review checks "does the table still look like the reference"
(13-art-direction.md defines what to look for).

## 6. Flake policy

- CI retries every non-determinism test once (`--repeat until-pass:2`). A
  test that passed on attempt 2 is a **flake occurrence**; the PR author
  must record it in `docs/JOURNAL.md` (test name, job, run link) before
  merging.
- Second occurrence of the same test within 30 days ⇒ **quarantine**: append
  the exact `suite.case` name (one per line) to `tests/quarantine.txt`, with
  a JOURNAL.md entry stating suspected cause and the milestone by which it
  will be fixed. CMake reads `quarantine.txt` at configure time (guarded by
  `if(EXISTS)`, empty list ⇒ nothing happens) and marks those tests
  `DISABLED` — applied through the generated CTest include of §2's snippet,
  because `PRE_TEST` discovery creates the gtest names only when CTest loads
  the test file. `weekly-deep` still runs them informationally by setting
  `TB_RUN_QUARANTINED=1`, which the generated include honours (see YAML).
- A quarantined test must be deflaked (and removed from the file) within two
  milestones or deleted with a JOURNAL.md justification. More than 5 entries
  in `quarantine.txt` blocks further quarantines — fix the flakiest first.
- Determinism tests are exempt from all of this: they never retry and never
  quarantine. A flaky `det_` test is a live determinism bug and blocks
  merge.

## 7. Coverage philosophy

No percentage gate, no coverage tooling in CI. Binding rules:

1. Every bug fix ships in the same PR with the test that would have caught
   it — the test must fail on the pre-fix code (state this in the PR
   description) and pass after.
2. Every V-code, every FT scenario, every `tb.*` API function, and every
   element type has at least one test by the milestone that ships it
   (04-milestones.md acceptance criteria enumerate them).
3. If a bug is genuinely untestable deterministically (e.g. a wall-clock
   race outside the sim), the fix adds the nearest possible guard (a
   `TB_CHECK`, a stress test) and a JOURNAL.md note explaining why.

## Common pitfalls

- **Copying expected values from the code's own first run.** That freezes
  bugs into the suite. Unit expectations come from hand arithmetic (§2.1, or
  your own shown in a comment); only goldens come from a run, only via
  §2.4.4.
- **Comparing determinism or synth goldens across OSes.** Cross-platform
  bit-exactness is a canon non-goal. Same-OS only (ADR-013). A Linux golden
  failing on Windows is not a bug; a Linux golden failing on Linux is.
- **Testing physics through the renderer.** Every sim test constructs
  `tb_sim` objects directly, headless. If a test needs `tb_render` or an SDL
  window to observe a physics fact, the test is wrong (or `tb_sim` is
  leaking a dependency, which the CMake layering forbids).
- **Letting tests touch the wall clock.** Ticks are the only time axis. A
  test that sleeps or polls wall-clock to detect a sim condition is flaky by
  construction; drive ticks in a loop and assert state.
- **Retrying determinism tests.** `--repeat until-pass:2` must never apply
  to `det_` tests; the YAML runs them in a separate non-retried step.
  Passing on retry means nondeterminism — the exact bug the test exists to
  catch.
- **Using `pull_request_target` for build-test.** That event exposes secrets
  to PR-controlled build scripts (CMake runs arbitrary code). Build/test
  uses plain `pull_request`; only the pinned review workflow uses
  `pull_request_target`, and it never builds the tree.
- **Passing `-R` without `-E '^$'` against a preset that excludes the
  category.** A CLI `-R` replaces only `filter.include.name`; every test
  preset still excludes `^(fuzz_sim_|perf_|bounds_)` (§4.1), so
  `ctest --preset release -R '^perf_'` selects zero tests and
  `--no-tests=ignore` turns that into a green required check — a perf gate
  that never runs and never complains. Pair every `-R` with `-E '^$'`, and
  prove it with `-N` in the PR that lands the category's first test (§3.2).
- **Opening a fixture by a path relative to the process working directory.**
  Under CTest the cwd is the preset's `binaryDir`, not the repo root, so
  `fopen("tests/fixtures/…")` passes for whoever ran the binary by hand and
  fails in CI. Everything repo-relative — fixtures, goldens, `assets/`,
  `tables/` — goes through `tb::test::data_path()` on `TB_SOURCE_DIR` (§2).
- **Reading 06-rendering.md §17.1's "exceeding a budget is a CI failure" as
  covering the GPU rows.** Hosted runners are software-rasterized: GPU ms and
  VRAM are hardware-only and surface at M19, while draw calls, SDF instances
  and the 1.5 ms particle CPU cost are runner-independent and do hold in CI
  (§2.9). Trying to make llvmpipe meet the 4 ms GPU budget ends in a widened
  threshold that gates nothing.
- **Forgetting `--no-tests=ignore` on filtered ctest steps.** Before a
  category's milestone lands, `-R '^det_' -E '^$'` matches nothing and
  `noTestsAction: error` fails the job. The main step must error on zero
  tests; the filtered steps must not.
- **Gating on a single perf run.** Shared runners jitter; one p99 sample is
  noise. Median of 3 runs, `-j1`, CI thresholds at 08-physics.md §9's 2×
  margin over the reference budget, changes only by ADR.
- **Writing an FT scenario as a replay tape.** A `[tick, bitmask]` tape pins
  the press to an absolute tick; FT-02 and FT-07 press on a *state*
  predicate (`ball.y ≤ 0.26`, `ball.y ≤ 0.175`) whose tick moves whenever a
  physics constant is tuned. Build the 08 §5.6 rig in code and inject on the
  predicate (§2.5). No `.tbreplay`, no `.replay.json`, no table fixture for
  feel tests — ever.
- **Asserting "load fails" on a warning-severity V-code fixture.** 17 of the
  09 §8 codes are warnings: their fail pack must *load successfully* and
  emit the warning (`tb_validate` exit 0, `--strict` exit 2). Likewise, a
  V-code fixture is a pack **directory**, not one JSON file — V032–V039 need
  `rules.lua`/`art.json`/`audio.json` beside `table.json` and are exercised
  through the validator, never the loader (§2.2).
- **Registering per-table tool tests before the tools exist.** Tables ship
  from M5; `tb_autoplay`/`tb_validate` are stubs until M15. Unconditional
  registration turns every build-test job red for ten milestones. The loop
  lives inside `if(TB_TOOLS_READY)` (§2) and the option flips ON in the M15
  PR — never "temporarily" delete a table or a test to get green.
- **Gating score or mode bounds on the 300 s smoke run.** A `--seconds`
  session never ends a game, so `score.*`/`modes.*` are undefined there and
  a bound on them would be measured against garbage. Session-shape-
  independent metrics go in the smoke run, score/mode/shot bounds in the
  `--balls 3` `bounds_` tests, each at its declared skill (§2.8).
- **Letting a perf gate outrun its dependencies.** M2 has no table loader
  and M5 has no Lua; a gate that loads either before it exists is a gate
  that cannot run. The §2.9 ladder is synthetic scene → tables → rules, with
  the limits and the median-of-3 protocol identical at every rung.
- **Gating a CPU budget with a frame period.**
  `perf_particles.two_thousand_live_at_60fps` gates the 06-rendering.md
  §17.1 CPU-encode budget — **1.5 ms per frame, 9.0 % of a 16.67 ms
  refresh** (§2.9) — not the refresh itself. Swapping the two makes the gate
  ~11× looser and it passes on any hardware, forever. Its GPU half is
  evidenced by the M13 F12 capture and, from M19, by
  `perf_frame.gate_render_frame_time`; a skipped frame gate is not particle
  evidence.
- **Gating frame time on a software rasterizer, or deleting the gate
  because CI cannot run it.** lavapipe/WARP cannot meet a hardware frame
  budget; the driver exits 2 and CTest reports SKIPPED, which is a visible,
  auditable state. Widening the threshold until llvmpipe passes, or dropping
  the registration, both destroy the gate (§2.10b).
- **Requiring a matrix check by its bare job name.** Without
  `name: build-test (${{ matrix.os }})` all three matrix check runs report
  as `build-test`, and the three required contexts of §3 can never go green.
  Copy the final names from `gh pr checks` on the M0 PR.
- **Pixel-gating screenshots.** GPU output legitimately differs across
  drivers and backends. Screenshots are review artifacts for eyeballs, never
  `EXPECT_EQ` material.
- **Asserting the energy bound with actuators on.** Flippers, slings, pops,
  kickers, and magnets inject energy by design. The energy invariant holds
  only in the passive configuration (`set_actuators_enabled(false)`); with
  actuators on, only I1–I4 apply.
- **Hashing printed/rounded floats in `state_hash()`.** Use raw IEEE-754 bit
  patterns. Rounding hides low-bit drift, precisely the signal the
  determinism suite must catch; the sim must be bit-stable, not
  approximately stable.
- **Resetting the event-sequence accumulator per tick, or skipping phase-3
  events.** The accumulator (§2.4.1) is initialized once at sim construction
  and never cleared; a per-tick reset would leave `state_hash()` sampling
  only the last tick's events and silently drop 999 ticks out of every
  1,000. Framework events (phase 3) and `timer_tick` (phase 4) absorb
  through the same sink as sim events — omit them and 11 §1's "the
  determinism suite compares those sequences" stops being true.
- **Gating shot rates at skill 2 only.** 14 §8.3 declares a skill-0 floor
  (`shots[<id>].rate` ≥ 0.02), so the `--balls 3` job runs skills 0, 1 **and**
  2 (§2.8b). Drop the s0 run and every table declaring that floor holds a
  bound no CI run evaluates — which §2.8 itself orders review to reject.
- **Applying the quarantine list with a configure-time
  `set_tests_properties()`.** The gtest names do not exist at configure time
  under `PRE_TEST` discovery, so CMake fails with "Can not find test to add
  properties to". Read the file at configure time, apply it from the
  generated CTest include (§2), and guard each entry with `if(TEST …)` so a
  stale line is skipped rather than fatal.

## Done when

- [ ] `.github/actions/tb-setup/action.yml` and
      `.github/workflows/build-test.yml` exist exactly as §3 (jobs `format`,
      `build-test` ×3 OS, `asan`, `perf-gates`, `weekly-deep`, `det-soak`),
      green at M0, coexisting with `zai-code-review.yml` (distinct trigger
      events and concurrency groups; both run on the same PR push).
- [ ] The `build-test` job carries `name: build-test (${{ matrix.os }})`, so
      `gh pr checks` on the M0 PR prints exactly the six §3 context strings.
- [ ] Those six checks are enforced on `main` by the 03-process.md
      branch-protection call (`required_pull_request_reviews: null`,
      `enforce_admins: false`), with the context names copied verbatim from
      `gh pr checks` — **or**, if protection cannot be configured, a
      JOURNAL.md entry records the fallback and the per-PR CI-green
      checklist is the enforcing mechanism. The `format` job fails on a
      deliberately misformatted file either way.
- [ ] From M19, `.github/workflows/release.yml` matches §3.3: pushing a
      `v*` tag produces the three packaged artifacts, each smoke-tested
      (unpack, `tiltburst --version` matches the tag, `tb_validate` on a
      bundled table) and attached to a draft GitHub release.
- [ ] `CMakePresets.json` matches §4.1; `cmake --preset debug|release|asan`
      work on Linux/macOS, `cmake --preset windows` on Windows;
      `ctest --preset <x>` excludes fuzz/perf/bounds by default.
- [ ] `tests/CMakeLists.txt` matches §2: the per-table `validate_`,
      `smoke_autoplay_`, and `bounds_autoplay_` registrations sit inside
      `if(TB_TOOLS_READY)` (default OFF, flipped ON in the M15 PR), so no
      milestone from M5 to M14 can go red on a stub tool; the §2.10 release
      gates sit behind `TB_RELEASE_GATES` (flipped ON in the M19 PR).
- [ ] Every `-R` step in §3.2 also passes `-E '^$'`, and
      `ctest --preset release -R '^perf_' -E '^$' -N` lists at least
      `perf_tick.gate_synthetic` from M2 (the same `-N` check run for
      `^fuzz_sim_` and `^bounds_` when their first tests land), so the
      `perf-gates` required check provably selects a non-empty set instead of
      passing on zero tests.
- [ ] `tb_tests` is built with `TB_SOURCE_DIR="${CMAKE_SOURCE_DIR}"` and every
      repo-relative path in every test goes through `tb::test::data_path()` —
      from M0's `FontAssets.VendoredFontsPresentAndParse` onward — so
      `ctest` passes from any working directory and no test carries a
      `WORKING_DIRECTORY` property (§2).
- [ ] The five worked examples of §2.1 exist as passing gtest cases with the
      exact given inputs, expected values, and tolerances.
- [ ] Every V-code has a `tests/fixtures/schema/<vcode>_pass/` pack and one
      `_fail[_<severity>]/` pack per declared severity;
      `unit_schema.vcode_coverage` fails when one is deleted;
      `unit_schema.vcode_fires_<code>` proves exactly that code fires, that
      warning-severity packs still load, and that V032–V039 are checked
      through the validator entry point rather than the loader (§2.2).
- [ ] `unit_synth.*` prove deterministic, non-silent, per-OS-golden PCM.
- [ ] `det_replay.twice_in_process_<slug>` runs 100k ticks twice for
      `test-lab` and every shipped table, comparing hashes each 1,000 ticks;
      `det_golden.same_os_<slug>` compares against per-OS goldens recorded
      by `tb_autoplay --record-golden`; `det_replay.flipper_tape_hash_stable`
      covers 04-milestones.md M4's committed flipper-heavy tape; CI uploads
      fresh goldens as artifacts on every run.
- [ ] `state_hash()` includes the §2.4.1 event-sequence accumulator (tick,
      `type`, `id` per event, in dispatch order, rolling and never reset), so
      reordering two events on a tick without moving a ball fails
      `det_replay.*` on every table — the promise 05 §6.3, 08 §2.2 rule 7,
      10 §2.2 and 11 §1 make.
- [ ] FT-01…FT-10 from 08-physics.md each map to a passing
      `feel_scenarios.ftNN_*` test built on the 08 §5.6 code rig (seed
      `0x54425354`, state-triggered injection, M8 additions only for
      FT-09/FT-10) — with no table fixture, no `.replay.json`, and no
      `.tbreplay` anywhere in the feel suite; `det_feel.twice_in_process_ft03`
      proves the rig is hash-stable.
- [ ] `fuzz_sim_invariants.*` run 1M ticks per shipped table in `perf-gates`
      (fixed seed) and weekly under ASan (randomized, logged seed); I1–I4
      and the passive energy bound are asserted every tick.
- [ ] All 15 loader corpus files exist; `unit_loader.malformed_corpus`
      passes ASan-clean.
- [ ] `script_api.golden_log` (cross-platform) and
      `script_testlab.golden_run` (per-OS) pass; sandbox tests prove the
      §2.7 restrictions.
- [ ] From M15: `validate_<slug>` runs `tb_validate <dir> --strict` and
      `smoke_autoplay_<slug>` runs a 300 s session at skill 1 for every
      `/tables` entry with exit 0, both invoked in the positional CLI form of
      09-table-format.md §10 / 14-authoring-guide.md §8.2 (no `--table`
      flag), gating the five §2.8(a) session-shape-independent metrics
      (`stuck_balls`, `drains.center`, `drains.outlane_share`,
      `coverage.share`, `ball_time_s.p50`).
- [ ] From M15: `bounds_autoplay_s{0,1,2}_<slug>` run
      `--runs 20 --seed 1 --balls 3 --check-bounds` in `perf-gates` (18
      tests: 6 packs × 3 skills) and gate `score.p50`, `modes.*`, and
      `shots[<id>].rate` at the skill each bound declares — including the
      14 §8.3 skill-0 shot-rate floor, so no declared bound goes unevaluated;
      the same bounds hold on 15 §0.7's binding 500-run acceptance sweep. A
      failing bounds test is answered by tuning — 15 §0.7's five-step list
      first for a shipped table, 14 §8.4's matrix for anything it does not
      cover (§2.8) — and only by widening a bound in the one documented case
      (sweep inside, 20-run prefix outside).
- [ ] `perf_tick.gate_synthetic` exists and is green **from M2** on the 88
      -collider code-built scene (no table.json, no Lua);
      `perf_tick.gate_tables` joins it at M5 and loads `rules.lua` from M9 —
      all at the §2.9 CI limits (mean < 100 µs, p99 < 200 µs, median of
      3 × 60,000 timed ticks, 4 balls, warmup discarded) — and a
      deliberately inserted 200 µs per-tick sleep fails the `perf-gates` job.
- [ ] `perf_latency.input_to_tick_p999` is green from M4: p99.9 < 4 ms over
      ≥ 10,000 scripted press edges through the real late-latch path.
- [ ] `perf_particles.two_thousand_live_at_60fps` is green from M13:
      headless, Release-only, median-of-3, 2,000 live particles × 600 steps
      at `dt` = 16.67 ms, per-frame particle CPU cost < **1.5 ms** (the
      06-rendering.md §17.1 budget, 9.0 % of a 60 Hz frame — never the
      16.67 ms period) with zero allocations, picked up by the existing
      `^perf_` step with no workflow edit (§2.9) — it is the only
      06-rendering.md §17.1 row a hosted runner can gate outright; the GPU-ms
      and VRAM rows are hardware-only evidence (§2.9 split table).
- [ ] All seven `perf_` gate ids in the plan are the seven of §2.9's
      inventory table — none coined elsewhere, none missing — which is what
      makes 04-milestones.md global rule 4's "gate ids are owned by
      16-testing-ci.md" true.
- [ ] From M19 the §2.10 release gates are green:
      `perf_startup.cold_boot_to_attract` < 1,500 ms (400 ms launch +
      1,100 ms headless boot), `perf_load.table_under_2s` < 2,000 ms per
      pack, and `perf_frame.gate_render_frame_time` either passes
      (p99 < 8.0 ms, max < 16.7 ms) or reports CTest **SKIPPED** with the
      software-rasterizer reason logged and a non-skipped local run pasted
      into the M19 PR.
- [ ] The §2.10(d) determinism soak has been run once at M19 and once at
      M20: `det-soak` dispatched on all three OSes, `--repeat until-fail:100`
      green, run URLs recorded in the M20 audit as the evidence for
      01-product.md §9's determinism metric.
- [ ] Renderer smoke passes or logged-skips on all three CI OSes, printing
      the §5 `render_smoke:` summary line; a forced crash in `--render-smoke`
      fails the job.
- [ ] `tests/quarantine.txt` machinery works exactly as §2's snippet: the
      `file(STRINGS)` read is guarded by `if(EXISTS)`, the empty file M0
      commits changes nothing and never fails configure, and an entry
      disables that test in PR jobs while `weekly-deep`
      (`TB_RUN_QUARANTINED=1`) still runs it.
- [ ] JOURNAL.md contains entries for every golden regeneration and every
      flake occurrence to date.
