# 04 — Milestones M0–M20

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 03-process.md (PR loop, conventions, splitting), plus the
per-milestone "Spec refs" listed below (01–16). PLAN.md §6 owns the milestone
list and titles; this document owns their exact scope.

## Global rules

These apply to every milestone; individual milestones do not restate them.

1. **One milestone = one PR**, branch `milestone/M<NN>-<slug>`, executed with
   the full review loop of 03-process.md §2. Milestones are strictly
   sequential; never start N+1 before N is merged. Splits only per
   03-process.md §5 (pre-authorized: M13, M17).
2. **Every PR:** green CI on Windows, Linux, macOS; tests included for all
   new behavior; CHANGELOG.md and docs/JOURNAL.md updated; no orphan TODOs;
   demo artifact attached to the PR body; per-PR checklist of 03-process.md
   §4 fully checked.
3. **Read before coding:** PLAN.md (fully), 03-process.md (fully), this
   milestone's section, and every doc in its "Spec refs" line. Spec docs are
   binding; spec errors are fixed via 03-process.md §3.3.
4. **Continuously enforced suites** (workflow content in 16-testing-ci.md):
   the determinism suite runs on every PR from M2 onward; the allocation-hook
   hot-path test from M2 onward; `tb_validate` + `tb_autoplay` over all
   shipped tables from M15 onward — their per-table CTest registration lives
   inside `if(TB_TOOLS_READY)` (CMake option, default OFF, flipped ON in the
   M15 PR: M15 Scope, D15); perf gates from the milestone that introduces
   each gate (`perf_tick.gate_synthetic` at M2; the table-based
   `perf_tick.gate_tables` from M5; `perf_particles.two_thousand_live_at_60fps`
   from M13). Gate **test ids are owned by 16-testing-ci.md** — the tick
   gates, the input-latency gate and the particle gate by §2.9, the three
   release gates by §2.10 — and are quoted verbatim here, never coined. Once green, a suite
   may never be removed or weakened.
5. **Size bands:** S ≈ under 800 added LOC, M ≈ 800–1,800, L ≈ 1,800–3,000.
   Above ~3,000 projected → split per 03-process.md §5.
6. **Requirement IDs** R1–R10 in acceptance criteria are PLAN.md §3.
7. **Test naming:** files `tests/<module>/<topic>_test.cpp`.
   **16-testing-ci.md §2 owns the naming convention.** The PascalCase
   `Suite.Case` labels listed per milestone name required *coverage*, not
   literal gtest ids: implement each under §2's
   `unit_`/`det_`/`feel_`/`script_`/`perf_` taxonomy, whose prefixes drive the
   CI filter regexes. An id already spelled in taxonomy form here — M0's
   `unit_scaffold.sanity` — is quoted verbatim from §2/§3.2, never re-coined.
8. Each milestone is self-contained: a fresh LLM session with PLAN.md, this
   file, and the cited spec docs must be able to execute it.

## M0 — Repository scaffold & CI

**Goal.** The repository builds all canonical targets and passes a trivial
test suite on all three OSes in CI.

**Why now.** Everything else depends on a reproducible build and the CI that
gates every future PR.

**Scope in:** build system, dependency manifest, CI workflow **plus every
file that workflow reads on the same commit** (the `tb-setup` composite
action, `tests/lsan.supp`, `tests/quarantine.txt`), formatting config,
doc/journal seeds, stub sources so every canonical target links, the three
vendored OFL fonts, the vendored `picosha2.h` their test hashes them with,
and `main` branch protection.
**Scope out:** any windowing, rendering, or simulation logic; SDL is a
declared dependency but not yet initialized.

**Tasks (ordered).**

1. Commit `/.clang-format` (exact content: 03-process.md §1.4) and
   `/.gitignore` (below).
2. Commit `vcpkg.json` (below) and pin the baseline. With a local vcpkg
   clone: `vcpkg x-update-baseline --add-initial-baseline`. **Without one**
   — no step of this plan requires a local clone — take the 40-char SHA from
   `git ls-remote https://github.com/microsoft/vcpkg HEAD` and write it into
   `builtin-baseline` by hand. The field is **not optional**: `tb-setup` runs
   `jq -r '."builtin-baseline"' vcpkg.json` and then `git checkout` on the
   result (16-testing-ci.md §3.1), so a missing field fails every job with a
   confusing `checkout null`.
3. Top-level `CMakeLists.txt` + per-module `CMakeLists.txt` creating all
   canonical targets (PLAN.md §5.1) with stub sources.
4. `CMakePresets.json`: byte-authoritative content in 16-testing-ci.md §4.1
   — copy it verbatim, never retype it.
5. `.github/actions/tb-setup/action.yml` — the composite setup action, exact
   content in 16-testing-ci.md §3.1, copied verbatim. **Every job that
   configures or builds** — `build-test` (×3 OS), `asan`, `perf-gates`,
   `weekly-deep`, `det-soak` — begins with
   `uses: ./.github/actions/tb-setup`; the only exception is `format`, which
   just checks out and installs clang-format-18 (16-testing-ci.md §3.2). A
   missing local action fails each of those jobs on its first step, before
   any compiler runs, so this file must land in the same commit as the
   workflow.
6. `.github/workflows/build-test.yml` — exact content in 16-testing-ci.md
   §3.2 (jobs `format`, `build-test` × {`windows-latest`, `ubuntu-latest`,
   `macos-latest`}, `asan`, `perf-gates`, `weekly-deep`; configure, build,
   `ctest`, clang-format check). These job names are the required checks task
   13 installs.
7. Test-support files that the presets and CMake read before any test runs —
   committed at M0 even though they are effectively empty, plus the CMake
   guards that keep them harmless:
   - `tests/lsan.supp` — LeakSanitizer suppression file. The `asan` preset
     sets `LSAN_OPTIONS=suppressions=${sourceDir}/tests/lsan.supp`
     (16-testing-ci.md §4.1); LeakSanitizer **aborts at process startup** if
     the named file does not exist, so the whole `asan` job dies without it.
     Commit it containing one comment line (`# no suppressions yet`).
   - `tests/quarantine.txt` — flake quarantine list (16-testing-ci.md §6),
     committed empty (a single trailing newline, no entries).
   - `tests/CMakeLists.txt` reads the quarantine list at **configure** time
     and must guard it:
     `if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/quarantine.txt")` around the
     `file(STRINGS ...)` read, and skip the `set_tests_properties(...
     DISABLED TRUE)` loop when the resulting list is empty. An empty or
     absent file must never fail configure or disable a test.
   - `tests/CMakeLists.txt` also carries the `TB_SOURCE_DIR` compile
     definition that `tb::test::data_path()` is built on, exactly as
     16-testing-ci.md specifies it. From M0 on, no test resolves a
     repo-relative path through the process working directory.
   - `tests/CMakeLists.txt` also declares
     `option(TB_TOOLS_READY "Register per-table tb_validate/tb_autoplay
     tests" OFF)` and wraps 16-testing-ci.md §2's per-table `foreach` loop in
     `if(TB_TOOLS_READY)`. It stays OFF until the M15 PR flips it (D15), so
     the tables that exist from M5 never invoke tool stubs and never turn CI
     red.
8. `README.md` stub: name, one-line description, the **local prerequisite the
   presets impose** (16-testing-ci.md §4.1) — clone and bootstrap vcpkg
   anywhere, then export `VCPKG_ROOT` (`export VCPKG_ROOT=/path/to/vcpkg`;
   PowerShell `$env:VCPKG_ROOT = "C:\path\to\vcpkg"`), since every configure
   preset names `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` as its
   toolchain file and every documented command below fails on a clean machine
   without it — then the build commands: Linux/macOS
   `cmake --preset release && cmake --build --preset release && ctest
   --preset release`, Windows `cmake --preset windows && cmake --build
   --preset windows-release && ctest --preset windows-release` (the presets
   are OS-conditioned; 16-testing-ci.md §4.1/§4.2) — pointer to PLAN.md,
   license note.
9. Seed `CHANGELOG.md` (03-process.md §1.2) and `docs/JOURNAL.md`:

```markdown
# Tiltburst Journal (append-only)

Newest entries at the bottom. Never edit or delete past entries;
corrections are new entries. Format: 03-process.md §3.1.
```

10. `LICENSE`: the repo already contains the Unlicense — keep it byte-for-byte
    unmodified; README states "Public domain (Unlicense)".
11. Minimal `src/main.cpp`: parses `--version`, prints
    `tiltburst <version>` from `src/core/version.h`, returns 0. Each `tb_*`
    library gets one anchor `.cpp` so it links. **Any other flag** — including
    the `--render-smoke --frames 120 --screenshot-dir …` invocation the M0
    workflow already runs — prints the one-line usage string to `stderr` and
    exits **2**. Exit 2 is the "not available / skipped" code that
    16-testing-ci.md §3.2 logs as a warning; any other nonzero code is read as
    a crash and fails the job. The full flag table is 05-engine-core.md §2 and
    lands with the features that implement it; M0 implements only `--version`
    and this exit-2 catch-all (05-engine-core.md §1–§2).
12. Vendor the three OFL fonts 13-art-direction.md §5.1 requires, so M13
    depends on files that already exist. Source: `github.com/google/fonts` at
    a **pinned commit**; copy `ofl/orbitron/Orbitron-Bold.ttf` (or the
    equivalent static instance under `ofl/orbitron/static/`),
    `ofl/monoton/Monoton-Regular.ttf`, and
    `ofl/righteous/Righteous-Regular.ttf` into `/assets/fonts/`, each with
    that family's `OFL.txt` (as `Orbitron-OFL.txt`, `Monoton-OFL.txt`,
    `Righteous-OFL.txt`). 13-art-direction.md §5.1 owns the two provenance
    artifacts and this file names them exactly: **`assets/fonts/SOURCES.md`**
    (upstream repo, the 40-char pinned commit, the fetch date, and the
    upstream path per file) and **`assets/fonts/SHA256SUMS`** (the verbatim
    `sha256sum` output over the three vendored `.ttf` files, in
    `sha256sum -c` format, one line per file). There is no `FONTS.md`.
    The **test** recomputes those digests in-process with the public-domain
    single-header `picosha2.h`, vendored in this same PR at
    `/tests/third_party/picosha2.h` with its provenance recorded like any
    other vendored asset (`/tests/third_party/SOURCES.md`: upstream repo,
    40-char pinned commit, fetch date, license). No crypto port joins
    `vcpkg.json` and nothing shells out to `sha256sum`, which windows-latest
    does not ship.
    Fallback if the download is impossible offline: substitute any available
    OFL geometric / display face, record the substitution as an ADR in
    02-decisions.md plus a
    JOURNAL.md entry, and never block on a download (03-process.md §3.2
    fallback matrix).
13. Configure branch protection on `main` per the branch-protection subsection
    of 03-process.md: push the M0 PR, run `gh pr checks` on it, copy the check
    names **verbatim** from that output (never from prose), then run the
    `gh api -X PUT repos/{owner}/{repo}/branches/main/protection` call given
    there with those contexts, `required_pull_request_reviews: null` (no human
    approval is ever required — canon: the implementor never asks a human) and
    `enforce_admins: false`. If the token lacks admin rights, proceed per the
    03-process.md §3.2 fallback row: the per-PR CI-green checklist is the
    enforcing mechanism; note it in JOURNAL.md.

**Files.**

```
/.clang-format  /.gitignore  /vcpkg.json  /CMakeLists.txt  /CMakePresets.json
/README.md  /CHANGELOG.md  /LICENSE (kept)  /docs/JOURNAL.md
/.github/workflows/build-test.yml
/.github/actions/tb-setup/action.yml   (16-testing-ci.md §3.1, verbatim)
/src/main.cpp  /src/core/version.h  /src/core/version.cpp
/src/{core,platform,sim,render,audio,table,game}/CMakeLists.txt + anchor .cpp
/src/tools/CMakeLists.txt  /src/tools/{tb_validate,tb_autoplay,tb_screenshot}_main.cpp (stubs printing usage, exit nonzero; real CLI contracts land at M15)
/tests/CMakeLists.txt  /tests/core/scaffold_test.cpp
/tests/lsan.supp        (comment-only; the asan preset points LSAN_OPTIONS at it)
/tests/quarantine.txt   (empty; read at configure time, guarded by if(EXISTS))
/tests/render/font_assets_test.cpp   (13-art-direction.md §5.1)
/tests/third_party/picosha2.h        (public-domain SHA-256, single header)
/tests/third_party/SOURCES.md        (upstream repo + pinned commit + fetch date + license)
/assets/fonts/{Orbitron-Bold.ttf,Monoton-Regular.ttf,Righteous-Regular.ttf}
/assets/fonts/{Orbitron-OFL.txt,Monoton-OFL.txt,Righteous-OFL.txt}
/assets/fonts/SOURCES.md   (upstream repo + pinned commit + fetch date + upstream path per file)
/assets/fonts/SHA256SUMS   (sha256sum -c format, one line per .ttf)
```

`vcpkg.json` (matches PLAN.md §5.2 exactly; SDL_shadercross is FetchContent
at M1, not a port — D9 moved the shader toolchain from M3 to M1):

```json
{
  "name": "tiltburst",
  "version-string": "0.1.0",
  "dependencies": [
    "sdl3",
    "lua",
    "sol2",
    "miniaudio",
    "nlohmann-json",
    "fmt",
    "stb",
    "gtest"
  ]
}
```

Top-level `CMakeLists.txt` structure:

```cmake
cmake_minimum_required(VERSION 3.28)
project(tiltburst VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions($<$<CONFIG:Debug>:TB_DEBUG>)
# static libs: tb_core tb_platform tb_sim tb_render tb_audio tb_table tb_game
add_subdirectory(src/core)      # links: fmt
add_subdirectory(src/platform)  # links: tb_core, SDL3
add_subdirectory(src/sim)       # links: tb_core ONLY (canon §5.1)
add_subdirectory(src/render)    # links: tb_core, tb_platform, SDL3
add_subdirectory(src/audio)     # links: tb_core, miniaudio
add_subdirectory(src/table)     # links: tb_core, tb_sim, nlohmann_json
add_subdirectory(src/game)      # links: tb_core, tb_sim, tb_table
add_executable(tiltburst src/main.cpp)   # links all of the above
add_subdirectory(src/tools)     # tb_validate, tb_autoplay, tb_screenshot (canon §5.1)
enable_testing()
add_subdirectory(tests)         # tb_tests (gtest)
```

`CMakePresets.json`: byte-authoritative content in 16-testing-ci.md §4.1 —
OS-conditioned Ninja `debug`/`release`/`asan` presets (`release` =
`CMAKE_BUILD_TYPE` `Release`) plus a Visual Studio `windows` preset, with the
matching build and test presets. Do not restate or re-derive it here.

`.gitignore`:

```
/build/
/out/
/vcpkg_installed/
.cache/
.idea/
.vs/
.vscode/
*.user
CMakeUserPresets.json
```

**Key interfaces.** `const char* tb::version_string();` in
`src/core/version.h` — nothing else yet.

**Tests.** `tests/core/scaffold_test.cpp`: **`unit_scaffold.sanity`** asserts
`tb::version_string()` is non-empty and matches `^\d+\.\d+\.\d+$`. This is the
first test id in the repo, its `unit_` prefix is what the CI filter regexes
select on, and 16-testing-ci.md (§2 taxonomy, §3.2 workflow note) owns and
already spells it — use it verbatim, in that case, with no PascalCase variant
anywhere. Without it the main `ctest` step hits the presets'
`noTestsAction: error`.
`tests/render/font_assets_test.cpp`: `FontAssets.VendoredFontsPresentAndParse`
— the assertions are 13-art-direction.md §5.1's and are implemented verbatim
from there: for each of the three `/assets/fonts/*.ttf` files the file exists,
its SHA-256 matches `assets/fonts/SHA256SUMS`, `stbtt_InitFont` succeeds on
its bytes, and the glyph/metric checks §5.1 lists pass; each `*-OFL.txt`
exists and is non-empty. The SHA-256 is computed **in-process** with the
vendored `/tests/third_party/picosha2.h` (task 12) — the assertion is exact
and must never be weakened to a size or a magic-number check, because
catching a truncated download or a committed LFS pointer at M0 is the whole
reason it exists. The `.ttf` files and `SHA256SUMS` are located with
`tb::test::data_path()`, never relative to the process working directory. It needs no GPU, so it runs in
every CI job — a truncated or LFS-pointer download fails here, at M0, not at
M13.

**Acceptance criteria.**

- [ ] CI green on all 3 OS (R8 groundwork, R10 groundwork) — including the
      `asan` job, which proves `tests/lsan.supp` exists, and configure, which
      proves the guarded `tests/quarantine.txt` read.
- [ ] All canonical targets from PLAN.md §5.1 configure, build, link.
- [ ] `tiltburst --version` prints `tiltburst 0.1.0` and exits 0; every other
      flag prints usage to `stderr` and exits 2, so the workflow's
      `--render-smoke` step logs a skip instead of failing.
- [ ] clang-format check job passes; LICENSE unmodified.
- [ ] The three OFL fonts + their OFL.txt files + `SOURCES.md` + `SHA256SUMS`
      are committed under `/assets/fonts/` and
      `FontAssets.VendoredFontsPresentAndParse` is green (R3 groundwork), or
      the substitution ADR + JOURNAL entry exists.
- [ ] `main` branch protection lists the M0 check names verbatim, with
      `required_pull_request_reviews: null` — or the "no admin rights"
      fallback is recorded in JOURNAL.md.

**Demo artifact.** CI run URL + pasted `tiltburst --version` output + the
`gh api .../branches/main/protection` response (or the fallback note).
**Size.** S (~600 LOC, mostly config, plus binary font files).
**Spec refs.** 03-process.md (branch protection, fallbacks),
16-testing-ci.md (§3.1 composite action, §3.2 workflow, §4.1 presets, §6
quarantine), 13-art-direction.md §5 (which fonts and why),
05-engine-core.md §1–§2 (exit codes).

## M1 — App skeleton & fixed-timestep loop

**Goal.** A window with an SDL3 GPU device clearing at native refresh, a
1000 Hz fixed-timestep sim thread publishing snapshots, and a timing overlay
drawn through a real (if minimal) shader pipeline.

**Why now.** The loop/threading/timing skeleton is the substrate every later
subsystem plugs into; latency instrumentation must exist before anything is
optimized (ARCHITECTURE.md §3). The overlay is the only evidence this
milestone can produce, and an overlay needs a pipeline, so the shader
toolchain lands here rather than at M3 (D9).

**Scope in:** SDL init; single window (windowed dev mode, 540×1080 portrait
default); GPU device creation + clear + present with frames-in-flight 1 and
MAILBOX-preferred present mode; **the shader build path** — FetchContent
SDL_shadercross at a fixed release tag, the per-(source, format) HLSL →
SPIR-V/DXIL/MSL custom commands of 06-rendering.md §16.4, the
`TB_COMPILE_SHADERS` option and the `/shaders/compiled/` committed-blob
fallback (ADR-012), and the runtime scan that passes only the formats
actually present to `SDL_CreateGPUDevice`; **one minimal quad pipeline**
(`sprite.vert.hlsl` + `sprite.frag.hlsl` per 06-rendering.md §16.3/§9, built
with the untextured flat-tint path — `stb_easy_font` emits colored quads,
no atlas) — exactly enough to draw the overlay and nothing more; sim thread
with fixed `dt = 0.001 s` accumulator (canon §5.3); triple-buffered
`SimSnapshot`; per-stage timing ring buffer + on-screen overlay (fps, tick
rate, frame ms, tick µs) rendered with `stb_easy_font` quads through that
pipeline (placeholder text until M13); logging (`stderr` + file in
`SDL_GetPrefPath`); config load `settings.json`; PCG32.
**Scope out:** SDF primitives, the projection/rotation path and debug draw
(all M3); any drawing beyond clear + overlay; input beyond quit/Escape;
physics.

**Tasks.** 1) core: `time.h`, `log.h`, `rng.h`, `config.h`. 2) platform: SDL
bootstrap, window, GPU device. 3) shader toolchain: FetchContent
SDL_shadercross at a fixed tag, `cmake/shaders.cmake` custom commands,
install to `<binary_dir>/shaders/`, `TB_COMPILE_SHADERS` + committed-blob
fallback. 4) the minimal quad pipeline + shader manifest loader (hardcoded
C++ manifest, no runtime reflection — 06-rendering.md §16.4). 5) sim:
snapshot buffer, sim thread, tick accumulator (spiral-of-death clamp: max 50
catch-up ticks/frame, then drop time with a warning). 6) render: clear +
overlay pass on the quad pipeline. 7) wire main loop.

**Files.** `src/core/{time,log,rng,config,assert}.{h,cpp}`,
`src/platform/{app,window,gpu_device}.{h,cpp}`,
`src/sim/{snapshot,sim_thread}.{h,cpp}`,
`src/render/{overlay,shader_load,pipeline_quad}.{h,cpp}`, `src/main.cpp`,
`cmake/shaders.cmake`, `/shaders/sprite.{vert,frag}.hlsl`,
`/shaders/compiled/` (fallback blobs, ADR-012),
`tests/core/{time,rng,config}_test.cpp`, `tests/sim/{snapshot,tick_loop}_test.cpp`,
`tests/render/shader_build_test.cpp`.

**Key interfaces.**

```cpp
// src/core/time.h
uint64_t ticks_now_ns();                      // monotonic; NEVER called from sim tick code
// src/core/rng.h  (full implementation verbatim in 05-engine-core.md §10)
class Pcg32 { public: void seed(uint64_t initstate, uint64_t initseq);
              uint32_t next_u32(); float next_float();
              uint32_t next_below(uint32_t bound);
              int32_t range_i32(int32_t lo, int32_t hi);
              float range_f32(float lo, float hi); };
// src/sim/snapshot.h
struct SimSnapshot { uint64_t tick = 0; /* grows in later milestones */ };
class SnapshotBuffer {                        // triple buffer, single writer
public: void publish(const SimSnapshot&);     // sim thread
        SimSnapshot acquire_latest() const;   // any reader, never blocks writer
};
// src/sim/sim_thread.h
class SimThread { public: void start(std::function<void(uint64_t tick)> tick_fn);
                          void request_stop(); void join(); };
```

The renderer-side types this milestone touches (`IRenderer`,
`RendererConfig`, `RenderFrame`, `make_sdl_gpu_renderer()`) are declared by
06-rendering.md §2 and are never restated here; M1 implements only the
device/present path and the one quad pipeline behind that interface.

**Tests.** `Pcg32.KnownSequence` (after `seed(42, 54)` the first three
`next_u32()` outputs are `0xa15c02b7`, `0x7b47f409`, `0xba1d3330` —
05-engine-core.md §10.1); `SnapshotBuffer.LatestWins` and `.NoTornRead`
(writer/reader threads, generation counter validated);
`TickLoop.Produces1000TicksPerSimSecond` (accumulator driven by a fake
clock); `Config.RoundTrip`; shader compilation is a **build step** from this
milestone on (a shadercross failure is red CI, not a test failure), plus
`ShaderBuild.ManifestMatchesInstalledBlobs` (every shader named in the C++
manifest has at least one compiled format installed next to the binary, or
the ADR-012 committed blob).

**Acceptance criteria.**

- [ ] Window opens on all 3 OS; overlay shows fps and tick rate (R1, R8
      groundwork).
- [ ] The overlay is drawn through the compiled quad pipeline (not a clear
      color): HLSL compiles at build time on all 3 OS, or the ADR-012
      committed blobs are used with a logged warning.
- [ ] Tick rate 1000 Hz ±0.1 % over 10 s wall time (overlay evidence).
- [ ] Frames-in-flight = 1 on the playfield swapchain; MAILBOX used when
      available, else VSYNC (log line evidence) (R2 groundwork).
- [ ] Clean shutdown: threads joined, no sanitizer complaints in Debug CI.

**Demo artifact.** Screenshot (OS capture) of the window with the overlay.
**Size.** M (~1,600 LOC, incl. the shader build path moved from M3).
**Spec refs.** 05-engine-core.md, 06-rendering.md (§2 boundary, §3
device/present, §9 sprite pipeline, §16.3/§16.4 shader layout and build),
16-testing-ci.md.

## M2 — Headless simulation core & determinism

**Goal.** A headless ball simulation with CCD against segments and arcs that
replays bit-identically from a recorded input stream.

**Why now.** Physics is the product's core risk (ARCHITECTURE.md ADR-003);
determinism must be locked in before any content exists.

**Scope in:** `Vec2` math; `Ball` state (canon: radius 0.0135 m, mass
0.08 kg); gravity `g·sin(slope)` along −y with default slope 6.5°; materials
(restitution, friction); colliders: segment, arc, point/post (circle);
ball–ball CCD (spheres); uniform-grid broadphase; sub-tick time-of-impact
loop; 12 m/s speed clamp; `SimState` + `state_hash`; input recording/replay —
the binary `.tbreplay` stream (format 05-engine-core.md §13) **and** the JSON
test tapes under `tests/fixtures/tapes/` that drive the determinism goldens
(16-testing-ci.md §2.4.2); `SimEvent` ring buffer skeleton;
allocation-free `Solver::step` with the allocation-hook test.
**Scope out:** flippers (M4), all table elements (M5–M8), rendering of any of
this (M3 draws it).

**Tasks.** 1) math + state types. 2) analytic sweeps (segment, arc, circle)
per 08-physics.md formulas. 3) broadphase grid. 4) TOI resolution loop with
restitution/friction response. 5) hash + replay recorder/player. 6) hook into
M1 sim thread; publish ball positions in `SimSnapshot`. 7) headless test
harness (no SDL — `tb_sim` links only `tb_core`, enforced by test).

**Key interfaces.**

```cpp
// src/sim/types.h
struct Vec2 { float x, y; };
struct Ball { /* declaration owned by 08-physics.md §1.2: index, live, pos, vel,
                 omega_z, layer, last_safe_pos, mode, plus the RAMP/CAPTURED
                 fields (ramp_elem, s, s_dot, holder_elem, hold_ticks) */ };
struct Material { float restitution; float friction; };
// src/sim/ccd.h
struct SweepHit { float toi; Vec2 normal; uint32_t collider_id; };
bool sweep_circle_vs_segment(Vec2 p0, Vec2 v, float r, Vec2 a, Vec2 b,
                             float max_t, SweepHit& out);
bool sweep_circle_vs_arc(Vec2 p0, Vec2 v, float r, Vec2 center, float radius,
                         float a0, float a1, float max_t, SweepHit& out);
bool sweep_circle_vs_circle(Vec2 p0, Vec2 v0, float r0,
                            Vec2 p1, Vec2 v1, float r1, float max_t, SweepHit& out);
// src/sim/solver.h
class Solver { public: void step(SimState& state, const TickInput& input); }; // dt = 0.001 fixed
uint64_t state_hash(const SimState&);   // FNV-1a over bit patterns of all dynamic state
```

**Tests.** `CcdSegment.NoTunnelAt12mps` (ball at clamp speed vs a 0.002 m
wall gap: never passes through, 10,000 random headings, fixed seed);
`CcdArc.TangentGrazeStable`; `CcdBallBall.HeadOnMomentum`;
`Broadphase.MatchesBruteForce` (1,000 random configs);
`Determinism.SameSeedSameHash` (10,000 ticks, two runs, identical hash — this
is the determinism gate 08-physics.md/16-testing-ci.md define, wired to run
on every PR from now on); `Determinism.ReplayFileRoundTrip` (record 5 s,
replay, hash equal); `Energy.PassiveBounceNeverGains` (restitution ≤ 1.0);
`HotPath.NoAllocationsInStep` (allocation hook, 16-testing-ci.md);
`Layout.SimIncludesNothingForbidden` (scans `src/sim/**` includes for
`render|platform|audio|SDL` — must find none);
**`perf_tick.gate_synthetic`** — the first rung of the perf ladder (D16), on
a scene built **programmatically in test code** (no `table.json`, no Lua —
neither exists yet): the M2 collider set, 4 balls (canon default) seeded
99, 5,000 warmup ticks untimed, 60,000 timed ticks, mean and p99 per run,
median-of-3 across runs, harness identical to 16-testing-ci.md §2.9. Limits
are §2.9's CI limits unchanged: **median-of-3 mean < 100 µs, median-of-3
p99 < 200 µs**. Release-only (`GTEST_SKIP()` unless `NDEBUG`), runs in the
`perf-gates` job. From M5 the same harness gains `test-lab` and every
shipped table as `perf_tick.gate_tables` (16-testing-ci.md §2.9 owns both
ids); M9 adds `rules.lua` loading.

**Acceptance criteria.**

- [ ] Determinism suite green and registered as a permanent CI gate (canon
      §5.3, R7 groundwork).
- [ ] No tunnelling at 12 m/s vs 2 mm geometry (ARCHITECTURE.md §1 table).
- [ ] `perf_tick.gate_synthetic` green in the `perf-gates` job: median-of-3
      mean < 100 µs and median-of-3 p99 < 200 µs (16-testing-ci.md §2.9). No
      table-based perf gate is registered at M2 — there is no table format
      until M5.
- [ ] Hot path allocation-free (test evidence in PR body).

**Demo artifact.** Committed input tape
`tests/fixtures/tapes/m2_bounce.replay.json` (format 16-testing-ci.md
§2.4.2) + its per-OS goldens under
`tests/golden/determinism/{windows,linux,macos}/m2_bounce.hashes` + test log
showing matching hashes. (Use the 16-testing-ci.md §2 fixture layout; there
is no `tests/data/` tree.)
**Size.** L (~2,000 LOC).
**Spec refs.** 08-physics.md (solver + CCD sections), 05-engine-core.md
(RNG, replay), 16-testing-ci.md.

## M3 — Renderer v1 & debug draw

**Goal.** The M2 simulation is visible: ball and colliders drawn as SDF
primitives, portrait rotation in the projection, holding 60+ fps.

**Why now.** M4 flipper tuning is impossible blind; debug draw must precede
feel work. The device, the shader build path, and the first pipeline already
exist from M1 (D9), so this milestone is purely about *what* is drawn.

**Scope in:** the SDF primitive pipeline — instanced SDF quads for
circles/segments/arcs per 06-rendering.md §8 (`sdf.vert.hlsl`/`sdf.frag.hlsl`
added to the M1 shader build); the projection/rotation path: ortho
`ViewTransform` with 0°/90° rotation mode and aspect-fit letterbox (rotation
in the projection matrix, never OS rotation — canon §5.9, 06-rendering.md
§6.2); debug draw of the `SimSnapshot` collider/ball set (F2 cycling per
06-rendering.md §16.1) delivered as part of `RenderFrame`; the M1 overlay
ported onto this path; F12 writes
`<SDL_GetPrefPath>/screenshots/tiltburst_YYYYMMDD_HHMMSS.png` via
`stbi_write_png` per 06-rendering.md §15.1 (the demo mechanism for all
milestones until tb_screenshot at M15).
**Scope out:** the shader toolchain and device/present path (both M1 — do not
re-do them here); TBArt, particles, bloom, real text (M13); backglass (M12).

**Tasks.** 1) `sdf.{vert,frag}.hlsl` + the SDF primitive pipeline, instance
buffers and batching (06-rendering.md §8). 2) `ViewTransform` ortho +
rotation + letterbox. 3) debug draw of `SimSnapshot` through `RenderFrame`.
4) port the M1 overlay onto the new path. 5) F12 capture path.

**Key interfaces.** None declared here. The renderer boundary —
`IRenderer`, `RendererConfig`, `RenderFrame`, `TableRenderData`,
`RenderStats` and the single factory `make_sdl_gpu_renderer()` — is owned by
**06-rendering.md §2** and implemented verbatim (same rule as `Ball` at M2).
`RenderFrame` is a *description* the game layer fills in, including the debug
toggles; there are no callbacks into game code and no immediate-mode
`begin_frame`/`draw_circle`/`end_frame` API anywhere in Tiltburst.

**Tests.** `Projection.PortraitRotationMapsCorners` (pure math: playfield
(0,0) and (0.52,1.04) map to the correct rotated pixel corners);
`Projection.AspectFitLetterboxes`; `RenderSmoke.DeviceClearPresent` — creates
a device, clears, reads back one pixel; **skips with a logged warning when no
GPU** (CI fallback, 03-process.md §3.2). Shader compilation remains a build
step from M1 (build failure = red CI).

**Acceptance criteria.**

- [ ] Ball + M2 collider set visible; motion smooth at native refresh.
- [ ] ≥ 60 fps with 300 debug primitives (overlay evidence) (R1 partial).
- [ ] 90° projection rotation verified against a reference screenshot (R4
      groundwork).
- [ ] F12 screenshot works on all 3 OS.
- [ ] `src/render/renderer.h` still declares exactly 06-rendering.md §2's
      `IRenderer` — no second renderer class, no immediate-mode drawing API.

**Demo artifact.** F12 screenshot of the ball among debug colliders, portrait.
**Size.** M (~1,100 LOC; the shader toolchain moved to M1 per D9).
**Spec refs.** 06-rendering.md (§2 boundary, §6 transforms, §8 SDF pipeline,
§15.1 F12, §16.1 debug draw), 05-engine-core.md, 07-displays.md (rotation
math only).

## M4 — Flippers, low-latency input & latency overlay

**Goal.** Flippers that can catch, cradle, and backhand, fed by raw input
with measured flipper-input→sim latency under 4 ms.

**Why now.** Flipper feel gates the whole project (ARCHITECTURE.md §11); it
needs sim (M2) and eyes (M3), and nothing else.

**Scope in:** flipper model per 08-physics.md (bespoke constraint: angular
velocity profile, ball–flipper CCD against the moving capsule, velocity
transfer, EOS bounce, catch damping); raw-input thread — Windows Raw Input
via hidden message-only window, Linux evdev (with permission-denied fallback
to SDL events + warning), macOS SDL events (canon §5.4); atomic latest-state
late-latched by the sim tick; default key bindings per 05-engine-core.md;
input timestamping at the raw event; latency overlay stages (input age at
tick, sim, record, GPU, present); nudge input plumbed (tilt logic is M10).
**Scope out:** plunger (M5), table format (M5), tilt penalties (M10).

**Tasks.** 1) flipper sim per 08-physics.md (angular profile, CCD vs moving
capsule, velocity transfer, EOS). 2) raw-input thread per platform + atomic
latest state. 3) late-latch wiring + event timestamps. 4) latency overlay
stages. 5) build the 08-physics.md §5.6 feel-test rig **in test code** and
implement FT-01…FT-08 on it (one gtest each).

**Key interfaces.**

```cpp
// src/platform/input.h  (design owned by 05-engine-core.md §9, verbatim there)
struct InputEdge {
    uint64_t ts_ns;      // tb::now_ns() timebase, taken at the OS event
    uint16_t action;     // fixed action index (05-engine-core.md §9.1 table)
    uint8_t  pressed;    // 1 press, 0 release
    uint8_t  source;     // 0 SDL, 1 WinRaw, 2 evdev, 3 synthetic, 4 replay
};
class InputSource {      // producers: SDLInputSource, WinRawInputSource, EvdevInputSource
public: virtual bool start() = 0; virtual void stop() = 0;
        virtual const char* name() const = 0;
        virtual size_t poll_edges(InputEdge* out, size_t max) = 0; // sim, in latch_input()
        virtual bool active() const = 0; };
// plus the shared std::atomic<uint32_t> g_button_bits latest-state that the
// sim tick late-latches (canon §5.4; edge-queue contract 05-engine-core.md §9.2)
// src/sim/flipper.h
struct FlipperParams {           // full field list + defaults in 08-physics.md
    Vec2 pivot; float length_m; float rest_angle_rad; float stroke_rad;
    /* omega profile, torque, EOS params per 08-physics.md */ };
class FlipperSim { public: void tick(bool button_held); float angle() const; };
```

**Tests.** All **FT-xx feel-test scenarios tagged M4 in 08-physics.md** —
FT-01…FT-08 (dead bounce, live catch, cradle hold, backhand, post pass, tap
pass, tip shot power, slap save); exact IDs, inputs and pass envelopes are
owned by 08-physics.md §5.7 and implemented verbatim. **08-physics.md §5.6 is
the normative harness**: a fixed rig built in test code — **no `table.json`,
no replay tape** — seed `0x54425354`, with **state-triggered** input
injection ("press when `ball.y` ≤ Y", the predicate evaluated after each tick
and the input injected for the next). One gtest case per scenario,
`feel_scenarios.ftNN_<name>` (16-testing-ci.md §2.5). There are no
`.tbreplay` FT fixtures and no `tests/data/replays/` directory.
`Determinism.FlipperReplayHashStable` — record → commit → replay a
flipper-heavy `.replay.json` test tape under `tests/fixtures/tapes/`
(16-testing-ci.md §2.4.2) and compare hashes. **This and
`det_feel.twice_in_process_ft03` (16-testing-ci.md §2.5) are distinct
coverage and M4 requires both:** the FT test re-runs the §5.6 *code rig*
twice in one process, proving flipper math is tick-deterministic with no
file involved; this test exercises the **replay machinery** — recorder,
committed tape, player, and the tape's absolute-tick input schedule — with
flippers active, which the in-process re-run never touches. Neither
substitutes for the other, and 16-testing-ci.md §2.5 states that split
explicitly — the code-rig test proves *the rig* is deterministic, this one
proves the *replay machinery* is.
`Latency.InputToTickUnder4ms` (synthetic timestamped edges through the real
late-latch path; asserts **p99.9 < 4 ms over ≥ 10,000 scripted press
edges** at tick consumption) — registered in the `perf-gates` job as
`perf_latency.input_to_tick_p999`, the id 16-testing-ci.md §2.9 owns;
`HotPath.NoAllocationsWithFlippers`.

**Acceptance criteria.**

- [ ] Every M4-tagged FT-xx scenario (FT-01…FT-08) green on the §5.6 code
      rig, bands unmodified (R7 partial).
- [ ] Flipper input → sim response **p99.9 < 4 ms over ≥ 10,000 scripted
      press edges** via the raw path on Windows/Linux dev runs; F3 latency
      overlay histogram export as evidence (R2, 01-product.md R2.1).
- [ ] Raw-input thread never blocks the sim; SDL fallback path works
      (macOS) (canon §5.4).
- [ ] Determinism suite still green with flippers active.

**Demo artifact.** Latency overlay screenshot (with the ≥ 10,000-edge sample
count and p99.9 visible) + the `feel_scenarios.*` test log listing each
FT-01…FT-08 band and the measured value. The rig is code, so there are no
replay files to commit.
**Size.** L (~2,200 LOC).
**Spec refs.** 08-physics.md (flippers, feel tests), 05-engine-core.md
(input, bindings), 16-testing-ci.md.

## M5 — Table format v1 & Neon Drift greybox

**Goal.** `table.json` loads into a playable table; the Neon Drift greybox
and the `test-lab` table exist and play with flippers and plunger.

**Why now.** Content stops being hardcoded here; every element milestone
(M6–M8) needs the loader and test-lab to grow against.

**Scope in:** `table.json` parse (nlohmann-json with
`ignore_comments=true` — canon §5.5), schema per 09-table-format.md: metadata,
play-area size, slope, materials, element list; element types this
milestone: `wall`, `post`, `flipper`, `plunger`, `outhole` (basic: drain →
auto-serve next ball), `light` (state only, drawn as debug circles); prefab
expansion engine + geometry prefabs used by the greybox
(`flipper_pair_standard`, `plunger_lane`, `inlane_outlane_pair`, `orbit`);
plunger sim (spring pull 0..1 → launch speed per 08-physics.md); load-time
validation with JSON-pointer-qualified errors; F5 hot-reload of the current
table; `tables/test-lab/` (minimal valid table, canon §5.8); Neon Drift
greybox layout (3 flippers) transcribed from 15-launch-tables.md geometry.
**Scope out:** all other element types (M6–M8), `rules.lua` (M9), `art.json`
(M13), `audio.json` (M11/M14); layers parse but layer 1 physics is M8.

**Tasks.** 1) schema structs + parser + path-qualified errors. 2) prefab
expansion engine + the four geometry prefabs. 3) `build_sim` + plunger sim.
4) author `tables/test-lab/`. 5) transcribe the Neon Drift greybox from
15-launch-tables.md. 6) F5 hot-reload.

**Key interfaces.**

```cpp
// src/table/table_loader.h  (load phase MAY throw — 03-process.md §1.6)
struct TableDef { /* full mirrored schema of 09-table-format.md */ };
TableDef load_table(const std::filesystem::path& table_dir);  // throws TableLoadError
class TableLoadError : public std::runtime_error {
public: const std::string json_pointer; const std::filesystem::path file; };
// src/table/sim_builder.h
void build_sim(const TableDef&, tb::SimState& out);           // allocates ALL pools here
```

**Tests.** `TableLoader.TestLabParses`; `TableLoader.EveryM5ElementRoundTrips`;
`TableLoader.BadFieldReportsJsonPointer` (error contains `/elements/3/pivot`);
`Prefab.FlipperPairExpansionGolden` (expanded primitive list matches a
committed golden JSON); `Plunger.PullToSpeedCurveMatchesSpec`;
`Determinism.NeonDriftGreyboxReplay` (60 s replay, stable hash); the M2
`perf_tick.gate_synthetic` harness gains its table-driven form
`perf_tick.gate_tables` here — same limits (median-of-3 mean < 100 µs,
p99 < 200 µs), now loading `test-lab` and every shipped table
(16-testing-ci.md §2.9; `rules.lua` loading joins at M9).

**Acceptance criteria.**

- [ ] `tiltburst --table tables/neon-drift` boots into the greybox; ball can
      be plunged, flipped, drains, re-serves (R9 partial, R5 groundwork).
- [ ] `tables/test-lab` loads and is used by the test suite (canon §5.8).
- [ ] Malformed table.json produces a path-qualified error and a clean exit,
      never a crash (03-process.md §1.6).
- [ ] F5 reloads an edited table in under 1 s without restart.
- [ ] `TB_TOOLS_READY` is still OFF: no per-table `tb_validate`/`tb_autoplay`
      CTest entry is registered, so the new tables cannot redden CI on tool
      stubs (D15; the flip is M15).

**Demo artifact.** F12 screenshot of the Neon Drift greybox + the 60 s
replay file.
**Size.** L (~2,500 LOC incl. table JSON).
**Spec refs.** 09-table-format.md, 15-launch-tables.md (Neon Drift), 08-physics.md
(plunger).

## M6 — Standard elements I

**Goal.** Slingshots, pop bumpers, standup targets, rollovers, gates, and
spinners simulate per spec and emit sim events.

**Why now.** These are the passive/reactive kinetic core of every table;
they only need M5's loader and M2's solver.

**Scope in:** element sims + `table.json` params for: `slingshot`,
`pop_bumper`, `standup_target`, `rollover`, `gate`, `spinner` (all physics
constants and behaviors per 08-physics.md; schema per 09-table-format.md);
`SimEvent` types for each (`switch_hit`, `spinner_spin`, `rollover`, … —
payloads per 10-scripting.md event table so M9 needs no rework); debug-draw
representation for each; test-lab extended with every new element.
**Scope out:** scoring/rules reactions (M9), sounds (M11), art (M13).

**Tasks.** 1) `EventRing` + event types. 2) slingshot + pop bumper impulse
elements. 3) standup, rollover, gate. 4) spinner. 5) debug draw, test-lab
additions, tests.

**Key interfaces.**

```cpp
// src/sim/events.h
enum class SimEventType : uint8_t { SwitchHit, TargetDown, SpinnerSpin, Rollover,
    KickerEnter, RampMade, Drain, /* grows in M7/M8 */ };
struct SimEvent { SimEventType type; uint64_t tick; uint16_t element_id;
                  uint8_t ball_id; float f0, f1; };
class EventRing { public: bool push(const SimEvent&);          // sim thread, lock-free
                          size_t drain(std::span<SimEvent> out); }; // consumer
```

**Tests.** `Slingshot.FiresOnBandCrossing` (impulse magnitude/direction per
08-physics.md, dead zone respected); `PopBumper.KickVectorRadial`;
`Standup.EmitsSwitchHitOnce` (debounce per 08-physics.md);
`Gate.OneWayBlocksReverse`; `Spinner.SpinCountFromBallSpeed` (speed→spins
curve golden); `Rollover.TriggersAtOverlap`;
`Determinism.TestLabAllElementsReplay`.

**Acceptance criteria.**

- [ ] All six element types live in test-lab and neon-drift where the 15
      layout places them (R7 partial).
- [ ] Every interaction emits its spec'd event with correct payload (verified
      by event-log golden tests).
- [ ] Determinism + hot-path suites green with all elements active.

**Demo artifact.** F12 screenshot of test-lab with all elements + a replay
showing sling/pop action.
**Size.** M (~1,600 LOC).
**Spec refs.** 08-physics.md (per-element sections), 09-table-format.md,
10-scripting.md (event payload table).

## M7 — Standard elements II

**Goal.** Kickers, drop target banks, captive balls, trough/outhole, ball
locks, and the ball-save mechanism complete the element set (minus
ramps/magnets).

**Why now.** These elements manage ball lifecycle (drain, serve, lock,
multiball) — prerequisites for rules (M9) and multiball tables.

**Scope in:** `kicker` (capture, dwell, scripted-strength eject),
`drop_target_bank` (individual targets, bank-complete detection, reset coil),
`captive_ball` (mini-ball constrained to a lane), `trough` (canon: 4 physical
balls; serve/drain accounting), full `outhole`, `ball_lock` (physical lock
+ release), ball-save timer mechanism (flag + auto-serve; policy scripted in
M9); multiple simultaneous active balls exercised (ball–ball CCD from M2);
events: `kicker_enter`, `target_down`, `bank_complete`, `drain`,
`ball_lock{lock_id, count}`, and `captive_full_travel{id}` — the captive
ball reaching the far end `b` of its slot at ≥ 0.3 m/s, on a **later** tick
than the strike's `switch_hit` (canon §5.7; 08-physics.md §6.13; payloads
10-scripting.md §4.1). It is not optional: 15-launch-tables.md §2.5 builds
Atomic Diner's SHAKE word and Milkshake Multiball on it. Test-lab extended
with every new element, captive ball included.
**Scope out:** multiball *rules* (M9), tilt (M10).

**Tasks.** 1) `BallManager` + trough/outhole accounting. 2) kicker
capture/dwell/eject. 3) drop target bank + reset. 4) captive ball. 5) ball
lock + ball-save mechanism. 6) multiball property/stress tests.

**Key interfaces.**

```cpp
// src/sim/ball_manager.h
class BallManager {              // owns trough accounting; part of SimState
public: uint8_t serve_to_plunger();            // returns ball_id or kInvalidBall
        void   drain(uint8_t ball_id);
        uint8_t add_ball_from_lock(uint16_t lock_element_id);
        int    active_count() const; int trough_count() const; };
```

**Tests.** `Kicker.CaptureDwellEject` (dwell ticks and eject vector per
08-physics.md); `DropBank.CompletesAndResets` (3-bank: three `target_down`
then one `bank_complete`; reset raises all); `CaptiveBall.StaysInLane`
(10,000-tick pounding, never escapes);
`CaptiveBall.FullTravelEmitsOnceAfterSwitchHit` (a strike hard enough to
drive the captive to the far end at ≥ 0.3 m/s emits `switch_hit` on the
impact tick and exactly one `captive_full_travel{id}` on a later tick; a weak
strike emits the `switch_hit` alone — 08-physics.md §6.13);
`Trough.CountsNeverGoNegative`
(property test: random serve/drain/lock sequences, invariant
`active + trough + locked == 4`); `BallSave.TimerServesWithinWindow`;
`MultiBall.ThreeActiveDeterministic`.

**Acceptance criteria.**

- [ ] All element types of canon §5.6 exist except `ramp`, `magnet`, `toy`
      (R7 partial).
- [ ] Ball accounting invariant holds under property tests — no lost or
      duplicated balls, ever.
- [ ] 3 simultaneous balls at 60+ fps with determinism green.

**Demo artifact.** Replay of a 3-ball juggle in test-lab + screenshot.
**Size.** M (~1,700 LOC).
**Spec refs.** 08-physics.md, 09-table-format.md.

## M8 — Ramps, layers & magnets

**Goal.** 2.5D play: ramps carry the ball between layers, upper playfields
work, and magnets feel right.

**Why now.** Completes R7's element list; the last physics milestone, so the
determinism and feel gates are re-locked here before content scales.

**Scope in:** `ramp` as constrained 1-D path with width + height profile
(canon §5.6): entry capture, path following with gravity from the profile,
exit release, fall-off rules; `layer: 1` free 2-D upper playfields with layer
masks on colliders/elements; z/vz integration on ramps (canon §5.3); `magnet`
(field force, `magnet_on/off/pulse` mechanics; script control lands M9 but
the sim API is final now); `toy` (static collider + visual anchor); Neon
Drift drift-corner magnet + ramps added to the greybox per 15-launch-tables.md.
**Scope out:** wireform art (M13 — wireforms are ramps with different art,
canon §5.6).

**Tasks.** 1) `RampPath` capture/follow/exit + z integration. 2) layer masks
through solver + broadphase. 3) magnet force + pulse. 4) `toy`. 5) Neon
Drift ramps + drift corner. 6) the M8-tagged FT-09/FT-10 scenarios.

**Key interfaces.**

```cpp
// src/sim/ramp.h
struct RampPath { std::vector<Vec2> points; std::vector<float> height_m;
                  float width_m;
                  // Per-end seam layer, DERIVED at load from the height
                  // profile by 08-physics.md §6.10.2's seam_layer(z_end)
                  // ([0] = s0 end, [1] = sS end); 0xFF marks an internal end
                  // (drop_exit / VUK feed), which carries no seam.
                  uint8_t seam_layer[2]; };
// There is no exit_layer field and no exit_layer JSON key: the exit layer is
// seam_layer(z(1)), derived the same way (08 §6.10.6; 09 §4.21 treats a
// written `exit_layer` as unknown key V026). The element's own `layer` is
// the ramp's ENTRY layer (0 in v1) and never places a seam.
// Ball's RAMP-mode fields (mode, ramp_elem, s, s_dot) per 08-physics.md §1.2
// src/sim/magnet.h
class MagnetSim { public: void set_active(bool);  void pulse(uint32_t ticks);
                          void apply(Ball&) const; };  // force per 08-physics.md
```

**Tests.** `Ramp.ClimbConservesPlausibleEnergy` (entry speed vs exit height
within 08-physics.md tolerance); `Ramp.SlowBallRollsBack` (below-threshold
entry returns to entry layer); `Ramp.RidesDownFromLayer1` (the derived-seam
case: layer-1 ball binds at the far end, rides down, unbinds on layer 0 —
08-physics.md §6.10.2/§6.10.6); `Layer.MasksIsolateColliders` (upper-layer
ball ignores layer-0 walls beneath); `Magnet.CaptureEnvelope` and the two **feel-test
scenarios tagged M8 in 08-physics.md §5.7** — **FT-09 Magnet catch and
throw** and **FT-10 Ramp make and rollback** (inputs and pass envelopes
owned by 08-physics.md §5.7, implemented verbatim, on the same §5.6 code rig
as M4 plus its M8 additions `magnet_m` and `ramp_r` — no table.json, no
replay tape);
`Determinism.FullElementSetReplay` (every element type in one test-lab
replay, the permanent full-coverage determinism gate).

**Acceptance criteria.**

- [ ] Ball rides a Neon Drift ramp to an upper region and returns (FT-10 Ramp
      make and rollback); drift corner magnet catches and throws per FT-09
      Magnet catch and throw (R7 complete at element level).
- [ ] **Ramps run downhill too:** on a `drop_exit: false` ramp whose final
      keyframe z = `playfield.layer1_z`, a FREE ball on **layer 1** binds at
      that far seam, rides the path down, and unbinds on layer 0 at the s = 0
      end (08-physics.md §6.10.2/§6.10.6). Atomic Diner's counter return
      (M16) is exactly this path, so it is proven here, on the greybox.
- [ ] FT-09 and FT-10 green with the 08-physics.md §5.7 bands unmodified;
      full-element determinism gate green and permanent.
- [ ] 60+ fps with full greybox + ramps (R1).

**Demo artifact.** Replay of ramp → upper layer → magnet catch; screenshot
showing the upper layer rendered above layer 0.
**Size.** L (~2,200 LOC).
**Spec refs.** 08-physics.md (ramps, layers, magnets, feel tests),
09-table-format.md, 15-launch-tables.md.

## M9 — Lua scripting & Neon Drift rules v1

**Goal.** The full sandboxed `tb.*` API runs table rules deterministically on
the sim thread; Neon Drift plays a scored game start to finish.

**Why now.** Every element and event now exists; rules make them a game.

**Scope in:** Lua 5.4 + sol2; sandbox per canon §5.7 (no `io`, `os`,
`require`, `load`; `math.random`/`math.randomseed` replaced by
`tb.rng`/`tb.rng_range` per 10-scripting.md §1.2; instruction-count
watchdog on a budget shared across all handlers per tick — **10,000**
instructions at 1,000-instruction hook granularity, ADR-006 and
10-scripting.md §2.4); the **complete** canonical event list and action
list of PLAN.md §5.7 as currently written — every name, no additions, no
omissions, explicitly including `tb.award_extra_ball`, `tb.kick_hold`, and
`tb.rng_range`; payload schemas per 10-scripting.md; `tb.state` per-player
table with auto-swap;
`tb.timer`/`tb.cancel_timer` at tick granularity; script errors contained
(03-process.md §1.6); `tb.backglass`/`tb.show_message` write a
`BackglassModel` struct (rendered on the playfield debug overlay until M12);
`tables/neon-drift/rules.lua` v1 per 15-launch-tables.md (scoring, gear-shift
drop banks, drift-corner mode, basic multiball).
**Scope out:** music/sfx playback (M11 consumes the already-emitted sound
requests), player rotation/tilt (M10), backglass display (M12).

**Tasks.** 1) sandboxed Lua VM + watchdog. 2) event dispatch + payload
marshalling. 3) full `tb.*` action surface. 4) `tb.state` + timers.
5) `BackglassModel` plumbing. 6) Neon Drift `rules.lua` v1 + headless game
test.

**Key interfaces.**

```cpp
// src/sim/script_host.h  (sim-side; sol2 types do not leak into headers)
class ScriptHost {
public:
    void load(const std::filesystem::path& rules_lua, SimState&);  // load phase, may throw
    void dispatch(const SimEvent&);        // hot path: no alloc, protected call
    void on_tick(uint64_t tick);           // timers
    void set_current_player(int index);    // swaps tb.state
};
```

**Tests.** `Sandbox.IoOsRequireLoadAreNil`; `Sandbox.WatchdogKillsRunawayHandler`
(infinite loop handler → disabled + logged, sim continues);
`Api.EveryCanonNameExists` (iterates both PLAN.md §5.7 lists — events and
actions — and fails on a missing *or* extra name);
`Events.PayloadGoldenPerType` (each event type dispatched, Lua echoes payload,
matches golden); `Determinism.ScriptRngReplayStable` (rules using `tb.rng`
replay bit-identically); `NeonDrift.ScriptedGameReachesGameEnd` (headless:
canned input replay completes a 3-ball game with score > 0);
`perf_tick.gate_tables` now loads each table's `rules.lua` as well, so
script dispatch is inside the measured tick (D16, 16-testing-ci.md §2.9) —
limits unchanged.

**Acceptance criteria.**

- [ ] Entire canon §5.7 API implemented and tested (R9 partial).
- [ ] Neon Drift: full scored game start-to-finish, headless and windowed.
- [ ] A deliberately broken rules.lua cannot crash or hang the sim.
- [ ] Determinism suite green with scripting active (canon §5.7).

**Demo artifact.** Headless game transcript log (events + final score) +
gameplay screenshot with score in the overlay.
**Size.** L (~2,500 LOC incl. Lua).
**Spec refs.** 10-scripting.md, 15-launch-tables.md (Neon Drift rules),
08-physics.md (element script hooks).

## M10 — Game framework: players, tilt, high scores

**Goal.** A complete 1–4 player alternating game with nudge/tilt and
persistent high scores.

**Why now.** Rules exist (M9); this wraps them in the game lifecycle needed
by audio/attract/menus later.

**Scope in:** game state machine per 11-game-framework.md §2.1 (full
`GameState` enum: Boot, Attract [minimal placeholder screen], TableSelect,
Settings, GameStarting, BallReady, BallInPlay, BonusCount, PlayerChange,
HighScoreEntry, GameOver, Paused — TableSelect/Settings/Paused exist as
minimal placeholders until M18 fills them in); 1–4 players alternating
with per-player `tb.state` swap (canon §5.7); ball count (default 3 per
11-game-framework.md),
extra balls; nudge → sim impulse + tilt bob accumulator with decay, warnings,
tilt (flippers disabled, ball drains, bonus lost — thresholds per
11-game-framework.md); high scores as per-table top-10 files
`scores/<slug>.json` under `SDL_GetPrefPath("tiltburst", "tiltburst")`
(path canon §5.9; file layout 11-game-framework.md §7), seeded on first run
(or a missing/corrupt file) from `table.json` `meta.default_scores` **when
the pack declares it** — exactly 10 entries when present (09-table-format.md
§2, V028) — and left **empty** when it does not: the key is optional, there
is no built-in ladder, and `test-lab` (which the Boot scan sees, since it
lives in `/tables` from M5) declares none, so an empty top-10 is a valid,
expected state everywhere a list is shown (11-game-framework.md §7);
initials entry via flipper keys; Start button starts/adds players.
**Scope out:** menus/settings UI (M18), Duel mode (M18), attract polish (M14).

**Tasks.** 1) `GameMachine` states + transitions. 2) player rotation +
per-player state swap. 3) nudge/tilt accumulator. 4) bonus count + extra
ball. 5) high-score persistence + initials entry.

**Key interfaces.**

```cpp
// src/game/game_machine.h
enum class GameState : uint8_t { Boot, Attract, TableSelect, Settings,
    GameStarting, BallReady, BallInPlay, BonusCount, PlayerChange,
    HighScoreEntry, GameOver, Paused };    // owned by 11-game-framework.md §2.1
class GameMachine {
public: void update(const SimEventBatch&, const MenuInput&);
        GameState state() const;  int current_player() const;
        const PlayerScore& score(int player) const; };
// src/game/high_scores.h
struct HighScore { std::array<char, 3> initials; uint64_t score; };
class HighScoreTable { public: bool qualifies(uint64_t) const;
        void insert(HighScore); void save(); void load(); }; // top 10 per table
```

**Tests.** `GameMachine.TransitionTableGolden` (every legal transition,
table-driven; illegal transitions assert); `Players.FourPlayerRotationOrder`
(P1..P4 alternate per ball, `player_up` events fired);
`Tilt.WarningsThenTiltAtThreshold` (accumulator + decay per
11-game-framework.md constants); `Tilt.FlippersDeadAfterTilt`;
`HighScores.PersistRoundTripAndOrdering`;
`HighScores.SeedsDeclaredDefaultsElseStartsEmpty` (a pack declaring
`meta.default_scores` seeds exactly those 10 entries on a fresh profile or a
missing/corrupt file; test-lab, which declares none, starts with 0 entries
and the first posted score lands at rank 1 — 11-game-framework.md §7, no
built-in ladder); `ExtraBall.SamePlayerShootsAgain`.

**Acceptance criteria.**

- [ ] 4-player alternating game completes with correct rotation and scores
      (R6 partial — classic mode).
- [ ] Nudge moves the ball; over-nudge tilts with 2 warnings first (R7 feel).
- [ ] High scores survive restart; on a fresh profile a pack that declares
      `meta.default_scores` (neon-drift — every launch table declares ten,
      15-launch-tables.md §0.6) shows exactly those 10 seeded entries, and a
      pack that declares none (test-lab) shows an **empty** list that
      accepts the first posted score at rank 1 (11-game-framework.md §7);
      initials entry works with flipper keys.
- [ ] Determinism preserved: nudges are inputs in the replay stream.

**Demo artifact.** Screenshot of 4-player score overlay mid-game + high
score entry screen.
**Size.** M (~1,800 LOC).
**Spec refs.** 11-game-framework.md, 10-scripting.md (`player_up`,
`tilt_warning`, `tilt` events).

## M11 — Audio engine & SFX synth

**Goal.** Sub-10 ms audio: sfxr-style synthesized patches triggered
sample-accurately from sim events.

**Why now.** Sound is part of the input-feel chain (ARCHITECTURE.md §8);
events (M6–M9) now exist to trigger it.

**Scope in:** miniaudio device (target buffer ≤ 256 frames @ 48 kHz; fall
back to 512 with a logged warning per 12-audio.md); lock-free voice-pool
mixer (32 voices, steal-oldest); sfxr-style patch synth per 12-audio.md
(`audio.json` patch schema); sim-event→sound scheduling with sample-accurate
offsets computed from event tick vs. audio clock (canon §5.4: sounds from sim
events, never render frames); `tb.play_sound` wired; built-in default patch
set under `/assets/sfx/` for tables that omit sounds; volume config.
**Scope out:** tracker music (M14), per-table music.

**Tasks.** 1) device + callback + voice-pool mixer. 2) sfx synth per
12-audio.md. 3) `audio.json` patch loading. 4) tick→sample scheduler +
`tb.play_sound` wiring. 5) default patch set + latency measurement.

**Key interfaces.**

```cpp
// src/audio/audio_engine.h
class AudioEngine {
public: static std::unique_ptr<AudioEngine> create(const AudioConfig&);
        void submit(const SimEvent&, uint64_t event_tick);  // main thread pump
        PatchId load_patch(const SfxPatch&);                // load phase
        float measured_latency_ms() const; };
// src/audio/sfx_synth.h
struct SfxPatch { /* full parameter set per 12-audio.md */ };
void render_patch(const SfxPatch&, uint32_t sample_rate, std::vector<float>& out_pcm);
```

**Tests.** `SfxSynth.PatchRendersDeterministicPcm` (golden hash of rendered
PCM for 5 reference patches); `Scheduler.TickToSampleWithin1ms` (synthetic
clocks; event at tick T lands within ±48 samples of the mapped position);
`Mixer.VoiceStealOldestNoClick` (envelope release applied);
`Mixer.CallbackAllocationFree` (allocation hook on the callback path);
`AudioSmoke.DeviceOpens` (skips with warning if no audio device in CI).

**Acceptance criteria.**

- [ ] Measured output latency < 10 ms on dev hardware (log evidence in PR)
      (ARCHITECTURE.md §8).
- [ ] Flipper/sling/pop sounds fire from sim ticks — audibly tight, and
      scheduling test proves ±1 ms mapping.
- [ ] Audio callback is allocation- and lock-free (03-process.md §1.6).

**Demo artifact.** Log of measured latency + a 10 s WAV capture rendered
offline from a replay (`tb_tests --render_replay_audio`, test-only flag).
**Size.** M (~1,600 LOC).
**Spec refs.** 12-audio.md, 05-engine-core.md (clocks).

## M12 — Multi-display & backglass

**Goal.** Cabinet display topology works: auto-detected portrait playfield +
squarest backglass, independent pacing, persisted assignment.

**Why now.** The game loop, scores, and `BackglassModel` (M9/M10) exist to
display; doing this before the art pass lets M13 style both screens.

**Scope in:** display enumeration + heuristic per canon §5.9 (h/w ≥ 1.4 →
playfield candidate, largest wins; squarest of the rest → backglass; explicit
`displays.json` config beats heuristics; single display → playfield only);
borderless fullscreen on both; backglass window rendered from the main thread
at ~30 Hz with **non-blocking** swapchain acquire — skip the backglass frame
if not ready (canon §5.4); backglass content v1: scores, current player, ball
number, `tb.backglass` messages, high scores in attract; landscape-reported
portrait TVs handled by projection rotation (M3); windowed dev mode
unchanged.
**Scope out:** DMD/topper (out of v1 product scope), attract art (M14),
backglass beauty (M13).

**Tasks.** 1) enumeration + `assign_displays` heuristic + `displays.json`.
2) backglass window + non-blocking ~30 Hz path. 3) `BackglassModel` renderer
v1. 4) rotated-TV verification on real hardware layout. 5) pacing tests.

**Key interfaces.**

```cpp
// src/platform/display_topology.h
struct DisplayInfo { int index; int w, h; float aspect; bool is_portrait; };
struct DisplayAssignment { int playfield_display; int backglass_display; // -1 = none
                           bool playfield_rotated_90; };
DisplayAssignment assign_displays(std::span<const DisplayInfo>,
                                  const DisplayConfigOverride*);   // pure, testable
// src/render/backglass_renderer.h
class BackglassRenderer { public: // returns false when swapchain not ready (frame skipped)
                                  bool try_render(const BackglassModel&); };
```

**Tests.** `DisplayAssign.PortraitPlusSquare` (1080×1920 + 1280×1024 →
correct roles); `DisplayAssign.SingleDisplayPlayfieldOnly`;
`DisplayAssign.LandscapeReportedPortraitRotates`;
`DisplayAssign.OverrideBeatsHeuristic`; `DisplayAssign.ThreeDisplaysPicksSquarest`;
`BackglassPacing.SkipNeverBlocks` (mock swapchain always-busy: playfield
frame time unaffected, backglass frames skipped and counted);
`DisplaysJson.PersistRoundTrip`.

**Acceptance criteria.**

- [ ] R4 complete: auto-detection, manual override, single-display fallback,
      persistence to `displays.json`.
- [ ] Playfield holds native refresh with the backglass attached — pacing
      test + overlay evidence (R1; ARCHITECTURE.md ADR-004).
- [ ] Backglass shows live scores/messages during play and high scores in
      attract.

**Demo artifact.** Photo/screenshots of both windows (or two F12 captures) +
overlay showing playfield fps unchanged with backglass active.
**Size.** M (~1,500 LOC).
**Spec refs.** 07-displays.md, 11-game-framework.md (BackglassModel),
06-rendering.md.

## M13 — Art system, particles & Neon Drift beauty pass

**Goal.** The TBArt vector art system, particles, glow/bloom, and real text
make Neon Drift look shipped.

**Why now.** All geometry/rules are stable, so art anchors to final layouts;
displays exist so both screens get styled.

**Pre-authorized split** (03-process.md §5): **M13a** engine (TBArt renderer,
particles, bloom, text), **M13b** Neon Drift content + backglass art.

**Scope in:** `art.json` TBArt format per 13-art-direction.md (layered SDF
primitives, palettes, decal prefabs, light-insert visuals bound to `light`
elements); bloom/glow post chain per 06-rendering.md (threshold + separable
blur + composite; quality settings); the **optional CRT branch inside the
final composite** (06-rendering.md §12.5) — scanline and vignette formulas
owned by 13-art-direction.md §10, gated on the `render.crt` settings key
(05-engine-core.md §11.1), **off by default**, a user setting only (no table
may enable it), and strictly scanline + vignette: no barrel distortion, no
chromatic aberration, no phosphor mask; particle system (pools sized at load;
emitters bound to sim events: sling/pop/drain/ramp sparks) sized to sustain
**≥ 2,000 live particles at ≥ 60 fps** (01-product.md R3.2); SDF text via
stb_truetype atlas from the OFL fonts **already vendored at M0** under
`/assets/fonts/` (replaces stb_easy_font everywhere); ball rendering
(shaded circle + trail per
13-art-direction.md); Neon Drift full `art.json` (playfield + backglass) per
15-launch-tables.md and 13-art-direction.md palettes; attract screen uses
real text.
**Scope out:** music (M14), other tables' art (M16/M17).

**Tasks.** M13a: 1) TBArt schema + loader. 2) art renderer layers + light
inserts. 3) bloom chain, then the optional CRT branch in the final composite
(06-rendering.md §12.5) behind `render.crt` (05-engine-core.md §11.1),
default false. 4) particle system + event-bound emitters. 5) SDF
text + font atlas. M13b: 6) Neon Drift `art.json` (playfield + backglass).
7) attract/title text pass. 8) walk the **"Style checklist" section of
13-art-direction.md** item by item against four F12 captures — **full
playfield**, **lower third**, **attract**, **multiball** — committed under
`docs/audit/m13/{full,lower_third,attract,multiball}.png` and pasted into the
PR body with the checklist ticked line by line.

**Key interfaces.**

```cpp
// src/render/tbart.h
struct TbArt { /* mirrored art.json schema per 13-art-direction.md */ };
TbArt load_art(const std::filesystem::path& art_json);        // load phase, may throw
class ArtRenderer { public: void draw_layer(int layer, const SimSnapshot&); };
// src/render/particles.h
class ParticleSystem { public: void spawn(const ParticleBurst&); // from sim events
                               void update(float dt); void draw(Renderer&); };
```

**Tests.** `TbArt.SchemaRoundTripAllPrimitives`; `TbArt.UnknownPrimitiveIsLoadError`;
`Particles.PoolNeverExceedsCapacity` (spawn storm: caps, no allocation);
`Bloom.DisabledFallbackRenders` (low-quality path);
`Crt.OffByDefaultAndBranchMatchesSpec` (GPU CI-skip rule applies: with
`render.crt` false the composite output is bit-identical to the plain
06-rendering.md §12.5 path; with it true a pixel on a dark scanline row keeps
1 − 0.12 = **0.88** of its luminance, a corner pixel at `r_norm` ≥ 1.0 keeps
1 − 0.15 = **0.85**, and a corner pixel on a dark row keeps
0.88 × 0.85 = **0.748** — 13-art-direction.md §10);
`Text.GlyphMetricsGolden`;
`RenderSmoke.NeonDriftArtFrame` (GPU CI-skip rule applies);
**`perf_particles.two_thousand_live_at_60fps`** — the R3.2 evidence, and it
must not wait for M15's tools: a headless gtest that drives the CPU particle
system (06-rendering.md §13) with 2,000 simultaneously live particles for
600 update steps at `dt` = 16.67 ms (10 s of frames) and asserts the
per-frame particle update + instance build stays inside the
**06-rendering.md §17.1 CPU-encode budget of 1.5 ms** — 9.0 % of the 16.67 ms
60 Hz frame period — and that the pool never allocates (01-product.md R3.2,
verbatim). The frame period is *not* the budget: gating on 16.67 ms would be
~11× looser than §17.1 and would pass no matter how slow the system got.
Release-only, `perf-gates` job, median-of-3. **The gate id and that
discipline are 16-testing-ci.md §2.9's** — it lists
`perf_particles.two_thousand_live_at_60fps` with the other perf gate ids and
the id is quoted verbatim here, never coined (global rule 4); this milestone
owns only the harness description above. Plus the GPU half measured on the dev
machine by the **F12-capture protocol**: run
`tiltburst --table tables/neon-drift`, trigger multiball, hold the F1
overlay (which reports `particles live/spawned` and frame ms — 06-rendering.md
§16.2) until ≥ 2,000 live is showing, press F12, and commit the capture as
`docs/audit/m13/multiball.png`. From M19, `perf_frame.gate_render_frame_time`
(16-testing-ci.md §2.10b) covers the same GPU half on any runner with a
hardware GPU — it reports SKIPPED on software rasterizers, so it never
replaces this capture.

**Acceptance criteria.**

- [ ] Neon Drift renders with full art, glow, particles on playfield and
      backglass; the four required captures (full playfield, lower third,
      attract, multiball) satisfy **every** item of the "Style checklist"
      section of 13-art-direction.md, ticked line by line in the PR body
      (R3, 01-product.md R3.1).
- [ ] **≥ 2,000 live particles sustained at ≥ 60 fps** (01-product.md R3.2):
      `perf_particles.two_thousand_live_at_60fps` green **and** the F12
      capture `docs/audit/m13/multiball.png` shows the F1 overlay with live
      particles ≥ 2,000 and frame time ≤ 16.67 ms. tb_screenshot/tb_autoplay
      are stubs until M15 and are not evidence for this box.
- [ ] ≥ 60 fps with full art + particles on reference-class hardware (R1).
- [ ] CRT mode is **off by default** (`render.crt` false, 05-engine-core.md
      §11.1) and with it off the final composite (06-rendering.md §12.5) is
      unchanged; with it on, scanlines and vignette match 13-art-direction.md
      §10 exactly (0.88 on a dark scanline row, 0.85 at `r_norm` ≥ 1.0,
      0.748 where both apply) and nothing else is added — no barrel
      distortion, no chromatic aberration, no phosphor mask. No table can
      turn it on.
- [ ] `light` elements show scripted states (on/off/blink) through art
      inserts.
- [ ] stb_easy_font fully retired.

**Demo artifact.** Before/after screenshots (greybox vs beauty) of playfield
and backglass, plus the four `docs/audit/m13/*.png` style-checklist captures.
**Size.** L ×2 (M13a ~2,400 LOC; M13b ~1,200 LOC mostly art.json).
**Spec refs.** 13-art-direction.md (palettes, typography, "Style checklist",
§10 CRT formulas), 06-rendering.md (§12 bloom, §12.5 final composite + CRT
branch, §13 particles, §14.1 font atlas, §16.2 overlay, §17.1 CPU-encode
budget), 05-engine-core.md §11.1 (`render.crt`), 16-testing-ci.md §2.9
(`perf_particles.two_thousand_live_at_60fps`), 15-launch-tables.md,
01-product.md (R3.1/R3.2).

## M14 — Music tracker & attract mode polish

**Goal.** Per-table tracker music plays, and the attract mode is a real
show.

**Why now.** Audio engine (M11) + art/text (M13) are prerequisites; attract
mode sells every later table.

**Scope in:** tracker format per 12-audio.md (patterns, instruments referencing
sfx-synth patches, order list) in `audio.json`; playback on the audio thread,
sample-accurate row scheduling; `tb.play_music`/`tb.stop_music` wired with
ducking under SFX per 12-audio.md; Neon Drift music (main loop + multiball +
attract themes per 15-launch-tables.md); attract mode loop per
11-game-framework.md: cycling screens (title/art card, high scores, "press
start", table preview) with music and particles; instant interrupt on Start.
**Scope out:** menus/settings (M18).

**Tasks.** 1) tracker schema + player on the audio thread.
2) `tb.play_music`/`tb.stop_music` + ducking. 3) Neon Drift songs. 4) attract
screen cycle + interrupt. 5) loop-seam + row-timing tests.

**Key interfaces.**

```cpp
// src/audio/tracker.h
struct TrackerSong { /* schema per 12-audio.md */ };
class TrackerPlayer { public: void play(const TrackerSong&, bool loop);
                              void stop(); void duck(float gain, uint32_t ms); };
```

**Tests.** `Tracker.SongRendersDeterministicPcm` (golden hash, reference
song); `Tracker.RowTimingWithin1ms`; `Tracker.LoopSeamless` (no click at the
loop point: sample continuity check); `Attract.CyclesAndInterrupts`
(state-machine test: screens advance on schedule, Start exits within 1 frame);
`Duck.SfxDucksMusicAndRecovers`.

**Acceptance criteria.**

- [ ] Neon Drift plays looping music in game and attract; multiball switches
      the theme (R3 partial).
- [ ] Attract loop cycles all screens and interrupts instantly on Start.
- [ ] Audio callback still allocation-free with tracker active.

**Demo artifact.** 30 s WAV capture of attract music + screenshots of each
attract screen.
**Size.** M (~1,500 LOC).
**Spec refs.** 12-audio.md, 11-game-framework.md, 15-launch-tables.md.

## M15 — Validator, autoplay harness & screenshot tool

**Goal.** The three CLI tools that make table authoring autonomous:
`tb_validate`, `tb_autoplay`, `tb_screenshot`.

**Why now.** M16/M17 author four tables; the tools must exist first (R9's
tooling half).

**Scope in:** the three executables (targets exist since M0 as stubs), CLI
contracts below; `tb_validate` implements every validation rule in
09-table-format.md (schema, references, geometry sanity, reachability
warnings) plus `art.json`/`audio.json`/`rules.lua` load checks; `tb_autoplay`
runs the headless sim with the skill-0/1/2 flip-window flipper policy and
emits the report defined in 14-authoring-guide.md §8.2 (`ball_time_s`,
`drains`, `coverage`, `shots`, `score`, `modes`, … — field names, schema,
and target thresholds owned by 14 §8.2/§8.3); `tb_screenshot` renders
a table to PNG offscreen.

**CI wiring — the `TB_TOOLS_READY` flip (D15).** `tests/CMakeLists.txt`
registers the per-table autoplay and validator CTest entries (the
`foreach(table_jsons …)` loop of 16-testing-ci.md §2) **inside**
`if(TB_TOOLS_READY)`. `TB_TOOLS_READY` is a CMake `option(... OFF)` added at
M0 and **flipped ON in this PR** — the option default changes to ON in the
same commit that lands the real tools. Rationale: tables exist from M5, but
until the tools are real those tests would invoke stubs that exit nonzero
and turn CI red on a table that is perfectly fine. So: registration lands
with the tools, here, never earlier. From this merge on, every PR runs
`tb_validate` over all shipped tables and the `tb_autoplay --check-bounds`
smoke of 16-testing-ci.md §2.8 (`--skill 1 --seconds 300 --seed 7`) on
test-lab + neon-drift; the score/mode/shot bounds run in the separate
`--balls 3` CI job (D23, 09-table-format.md `meta.autoplay_bounds`).

CLI contracts. Each tool's CLI has exactly one **normative** owning spec;
this block only restates the owners for wiring and must never diverge from
them. 09-table-format.md §10 is the normative `tb_validate` contract (flags,
diagnostic format, exit codes). **14-authoring-guide.md §8.2 is the normative
`tb_autoplay` CLI *and* report contract** — flags, skill profiles, session
shapes, exit codes, and every report field name; 16-testing-ci.md §2.8/§3
only *invoke* it. **06-rendering.md §15.2 (with §15.3) is the normative
`tb_screenshot` contract** — flags, offscreen behavior, defaults, exit codes;
the review views come from 14-authoring-guide.md §8.5. Owners may extend
flags; this file never changes them:

```
tb_validate <table-dir> [--strict] [--json] [--migrate]
  exit 0 = no errors (warnings allowed); 2 = validation errors
  (--strict: warnings also exit 2); 3 = file/IO failure     (09 §10)
tb_autoplay tables/<slug> --runs N --skill {0|1|2} --seed S
            [--balls 3 | --seconds 300] [--report out.json]
            [--check-bounds] [--replay tape.replay.json]
            [--record-golden out.hashes]
  exit 0 = ran to completion (with --check-bounds: and all checks green,
  which additionally requires the 16-testing-ci.md §2.8 pass conditions);
  1 = sim/script error, stuck ball, or bounds violation;
  2 = usage/IO error                                        (14 §8.2)
tb_screenshot <table-dir> --out <png-or-dir> [--width 1080] [--height 1920]
              [--tick T] [--seed S] [--replay in.tbreplay] [--art-only]
              [--views full,lower,upper,backglass,attract]
              [--state <mode-id|multiball|wizard>]
  exit 0 = wrote PNG(s); 1 = bad arguments / table load or render error;
  2 = no GPU (CI treats as skipped)     (06 §15.2–§15.3; views 14 §8.5)
```

**Key interfaces.** The tools link `tb_table`/`tb_sim`/`tb_render` public
APIs only — no tool-private hooks into internals; anything a tool needs must
become a public API of its module.

**Tests.** `Validate.TestLabPasses`; `Validate.EachRuleFiresOnCraftedBadTable`
(one fixture **pack directory** per 09-table-format.md V-code —
`tests/fixtures/schema/<vcode>_{pass,fail}/` per 16-testing-ci.md; V032–V039
are exercised through `tb_validate`, not the loader);
`Validate.JsonOutputSchemaStable`;
`Autoplay.DeterministicMetricsForSeed` (same seed → identical report JSON);
**`Autoplay.Skill0KeepsBallAlive`** — `tb_autoplay tables/test-lab --skill 0
--seconds 300 --seed 7`, asserting **`ball_time_s.p50` ≥ 5 s and
`stuck_balls` == 0**. This band is test-lab's own: test-lab is a minimal
fixture, not a designed table, so 14-authoring-guide.md §8.3's **22–60 s**
`ball_time_s.p50` band — which is defined at **skill 1 for designed tables**
— does not apply to it and must not be cited here. (14 §8.3 is the authority
for that band; read it from there, never from this file.) The test proves the
harness keeps a ball alive and never wedges it, nothing more.
`Screenshot.WritesExpectedSizePng` (GPU CI-skip rule applies);
`Tools.ExitCodesPerContract`.

**Acceptance criteria.**

- [ ] All three tools run on test-lab and neon-drift per the contracts (R9).
- [ ] `tb_validate` covers every rule ID in 09-table-format.md (cross-checked
      by test fixtures).
- [ ] `tb_autoplay` report fields match the 14-authoring-guide.md §8.2
      schema exactly and are deterministic per seed.
- [ ] `TB_TOOLS_READY` defaults **ON** as of this PR, the per-table
      autoplay/validator CTest entries are registered, and CI validates all
      shipped tables on every PR from now on (global rule 4, D15).

**Demo artifact.** `tb_screenshot` PNG of Neon Drift + an autoplay report
JSON (14 §8.2 schema) for seed 1.
**Size.** M (~1,800 LOC).
**Spec refs.** 09-table-format.md (validation rules), 14-authoring-guide.md
(metrics), 06-rendering.md (offscreen render).

## M16 — Second table: Atomic Diner

**Goal.** Atomic Diner ships complete (layout, rules, art, sound, music) —
authored **strictly by following 14-authoring-guide.md**, dogfooding the
pipeline.

**Why now.** The first table proved the engine; the second proves the
*authoring pipeline*, which is R9/R10's core promise, before three more
tables depend on it.

**Scope in:** `tables/atomic-diner/` — all four files + rules per canon §5.8:
60s googie space-age diner theme, order-completion modes, captive-ball
milkshake, upper mini-playfield; authored by executing 14-authoring-guide.md
step by step **in order, without shortcuts**; every gap, ambiguity, or
friction found in the guide is fixed in 14-authoring-guide.md **in the same
PR** (03-process.md §3.3) — the PR body must list each gap found, or state
explicitly which steps ran friction-free; tuning via `tb_autoplay` until
14-authoring-guide.md metric thresholds pass; `tb_validate` clean.
**Scope out:** engine changes beyond genuine bug fixes surfaced by authoring
(each needs a test reproducing the bug first).

**Files.** `tables/atomic-diner/{table.json,rules.lua,art.json,audio.json}`,
updates to `docs/plan/14-authoring-guide.md`, test fixtures.

**Key interfaces.** None new (that is the point — if a new engine API is
needed, that is a spec gap: fix spec + ADR).

**Tests.** `AtomicDiner.ValidatePasses`; `AtomicDiner.AutoplayMetricsInBounds`
(14-authoring-guide.md thresholds); `AtomicDiner.ScriptedGameReachesGameEnd`;
`Determinism.AtomicDinerReplay`; upper mini-playfield exercised in the replay.

**Acceptance criteria.**

- [ ] Atomic Diner playable start-to-finish with rules, art, sound, music
      (R5 partial: 2 of 5).
- [ ] Authored using only 14-authoring-guide.md + spec docs; PR body lists
      every guide gap found and the same-PR guide fixes (R9, R10 evidence).
- [ ] All three tools pass on it; determinism green.
- [ ] Signature mechanics work: order modes, milkshake captive ball, upper
      mini-playfield (canon §5.8).

**Demo artifact.** `tb_screenshot` of the table + autoplay metrics JSON +
the list of guide gaps in the PR body.
**Size.** L (~2,000 LOC, mostly content).
**Spec refs.** 14-authoring-guide.md (primary), 15-launch-tables.md (Atomic
Diner design), 09/10/12/13 as referenced by the guide.

## M17 — Tables 3–5

**Goal.** Tilt-O-Tron, Cosmic Carnival, and Voltage Vandals ship, completing
the five-table lineup.

**Why now.** Pipeline is dogfooded (M16); this is content scale-out.

**Pre-authorized split:** three consecutive PRs, one table each —
**M17a** `tilt-o-tron`, **M17b** `cosmic-carnival`, **M17c**
`voltage-vandals` (branch/slug per 03-process.md).

**Scope in (each part):** the full table pack per its 15-launch-tables.md
design and canon §5.8 signature mechanics — Tilt-O-Tron: build-a-robot drop
banks, magnet crane ball lock, 4-ball multiball; Cosmic Carnival: cannon
skill shot (kicker + scripted aim per 15), spinner-heavy juggling multiball;
Voltage Vandals: timed heist modes, alarm magnet grid, risky outlane gates.
Authored per 14-authoring-guide.md (guide fixes same-PR if new gaps appear).
**Scope out:** engine feature work (same rule as M16).

**Tests (per table, same pattern as M16).** `<Table>.ValidatePasses`,
`<Table>.AutoplayMetricsInBounds`, `<Table>.ScriptedGameReachesGameEnd`,
`Determinism.<Table>Replay`, plus one test per signature mechanic (e.g.
`TiltOTron.FourBallMultiballStarts`, `CosmicCarnival.CannonSkillShotWindow`,
`VoltageVandals.HeistTimerFailsAndSucceeds`).

**Acceptance criteria.**

- [ ] All five canon §5.8 tables complete and passing tools + determinism
      (R5 complete).
- [ ] Each signature mechanic demonstrably works (named tests green).
- [ ] 4-ball multiball holds 60+ fps (R1) — Tilt-O-Tron replay evidence.

**Demo artifact (per part).** `tb_screenshot` + autoplay metrics JSON.
**Size.** L ×3 (~1,800 LOC each, mostly content).
**Spec refs.** 15-launch-tables.md, 14-authoring-guide.md, 09/10/12/13.

## M18 — Menus, settings, input remap & Duel mode

**Goal.** Full out-of-game UX: table select, settings, input remapping, and
the 2-player Duel mode.

**Why now.** All content exists to select and configure; last feature
milestone before hardening.

**Scope in:** menu system per 11-game-framework.md (navigable with flipper
keys + Start, cabinet-friendly): table select (art cards via TBArt), players
1–4, settings (display override UI writing `displays.json`, audio volumes,
quality toggles for bloom/particles), input remap (capture-next-key flow
writing the `input` block of `settings.json` — 05-engine-core.md §11.1 is the
single authoritative key list and there is no separate input file — covering
every action of 05-engine-core.md §9.1, per-device on the raw paths); **Duel mode** per 11-game-framework.md (2 players head-to-head:
simultaneous scoring windows, steal/attack rules as specified there);
pause/resume (sim freeze, not process freeze; determinism preserved).
**Scope out:** any new table content; online anything (non-goal).

**Key interfaces.**

```cpp
// src/game/menu.h
class MenuSystem { public: void update(const MenuInput&);
                           void draw(Renderer&, const TbArt& ui_art);
                           std::optional<GameLaunch> take_launch_request(); };
struct GameLaunch { std::string table_slug; int player_count; GameMode mode; };
enum class GameMode : uint8_t { Classic, Duel };
// src/platform/input_remap.h
struct InputBinding { uint32_t button; /* per 05-engine-core.md */ uint32_t scancode; };
class RemapSession { public: void begin(uint32_t button);
                             bool feed(uint32_t scancode); }; // true = captured
```

**Tests.** `Menu.NavigationReachesEveryScreen` (graph walk, no dead ends);
`Remap.CaptureAssignPersistRoundTrip`; `Remap.ConflictRejected` (same key,
two buttons); `Duel.RulesPerSpec` (scoring windows/steals per
11-game-framework.md, table-driven); `Pause.SimFreezeDeterministic` (pause
mid-replay, resume, hash unchanged); `Settings.QualityTogglesApplyLive`.

**Acceptance criteria.**

- [ ] R6 complete: 1–4 alternating + Duel mode, launchable from the menu on
      the cabinet layout.
- [ ] Every input rebindable and persisted; defaults restorable.
- [ ] Display override UI covers all R4 cases without editing JSON by hand.
- [ ] Menus fully usable with only cabinet buttons (no mouse).

**Demo artifact.** Screenshots: table select, settings, remap capture,
Duel-mode HUD.
**Size.** L (~2,200 LOC).
**Spec refs.** 11-game-framework.md (menus, Duel), 07-displays.md,
05-engine-core.md (bindings).

## M19 — Performance hardening & packaging

**Goal.** All perf gates locked at release thresholds, and installable
artifacts for the three OSes come out of CI.

**Why now.** Optimize once, after all features exist; package what is final.

**Scope in:** profiling pass over sim tick, render frame, table load and cold
start (tooling per 16-testing-ci.md); fix regressions until **the tick gate
of 16-testing-ci.md §2.9 plus the three release gates of §2.10** are green —
`perf_tick.gate_tables` (sim tick, every shipped table, §2.9), and from
§2.10: `perf_startup.cold_boot_to_attract` (process launch + headless
bootstrap → Attract), `perf_frame.gate_render_frame_time` (frame time;
registered everywhere, reports SKIPPED on runners without a hardware GPU or
on a software rasterizer), and `perf_load.table_under_2s` (full pack load,
one case per pack). **16-testing-ci.md owns every one of those ids and
thresholds — §2.9 the tick limits, §2.10 the release numbers — and this file
states no number and invents none:** the 2-second pack-load budget lives in
the §2.10c gate, not here and not in 09-table-format.md, and a budget that
only exists in this file is not a gate. Also: allocation audit (hot-path test
extended to full gameplay on all five tables); packaging per OS —
Windows: `.zip` with `tiltburst.exe` + assets +
tables + vc-redist note; macOS: `.app` bundle in a `.dmg` (unsigned;
Gatekeeper right-click-open note in README); Linux: `.tar.gz` with launcher
script; CI `release.yml` workflow (content in 16-testing-ci.md) builds and
uploads all three artifacts on tag push; packaged-build smoke test in CI
(unpack, run `tiltburst --version`, run `tb_validate` on a bundled table);
version stamping from the git tag.
**Scope out:** code signing/notarization (documented as post-1.0), installers
with GUIs.

**Key interfaces.** None new; `tools/package/` scripts (`package_win.cmake`
etc. or CPack config per 16-testing-ci.md).

**Tests.** `perf_tick.gate_tables` (16-testing-ci.md §2.9) plus the three
§2.10 release gates — `perf_startup.cold_boot_to_attract`,
`perf_frame.gate_render_frame_time`, `perf_load.table_under_2s` — at their
release thresholds, green on every PR from now on (the frame-time gate may
report SKIPPED on a software-rasterized runner, never deleted; the cold-start
box is `perf_startup.cold_boot_to_attract` against §2.10's threshold — the
only cold-start gate that exists, and the only budget for it);
`TB_RELEASE_GATES` flips ON in this PR, exactly as `TB_TOOLS_READY` did at
M15;
`Package.SmokeBootsFromArchive` (CI job, all 3 OS);
`Package.ContainsAllTablesAndAssets` (manifest check).

**Acceptance criteria.**

- [ ] `perf_tick.gate_tables` (16-testing-ci.md §2.9) and the three §2.10
      release gates — `perf_startup.cold_boot_to_attract`,
      `perf_frame.gate_render_frame_time`, `perf_load.table_under_2s` — all
      green at their owning section's thresholds, with the frame-time gate's
      non-skipped local run pasted into the PR (R1, R2).
- [ ] Packaged builds for all 3 OS produced by CI from a tag and smoke-boot
      on clean runners (R8).
- [ ] Hot-path allocation audit clean across all five tables.
- [ ] No perf gate was weakened to pass, and no threshold was changed without
      an ADR in 02-decisions.md (03-process.md fallback matrix,
      16-testing-ci.md §2.9/§2.10). A gate that SKIPs is not a gate that
      passes (16-testing-ci.md §2.10b).

**Demo artifact.** CI artifacts page link + the three archive file listings +
perf gate summary table pasted in the PR.
**Size.** M (~1,500 LOC).
**Spec refs.** 16-testing-ci.md (gates, release workflow), 07-displays.md
(cabinet verification list).

## M20 — Release 1.0

**Goal.** The Definition-of-Done audit from PLAN.md §8 passes with recorded
evidence; v1.0.0 is tagged.

**Why now.** Everything ships before it; this milestone proves it.

**Scope in:** produce `docs/RELEASE-1.0-audit.md` containing the evidence
table below, one row per PLAN.md §8 checkbox and per requirement R1–R10;
every evidence cell is a concrete link/name: a CI run URL, a test name that
is green, a committed screenshot path, a JOURNAL entry, or a metrics JSON.
Fix any audit failure before proceeding (audit failures reopen as targeted
fix commits in this PR, or — if > S-sized — as an `M20a` split). Update
CHANGELOG (`[Unreleased]` → `[1.0.0] - <date>`), bump version to 1.0.0, tag
`v1.0.0` after merge (tag push triggers the M19 release workflow), final
JOURNAL entry.
**Scope out:** new features of any kind.

**Evidence table format (mandatory):**

Every evidence cell names a **gate or test that exists**; no row may cite an
undefined "budget per <doc>". The perf rows quote the gate ids verbatim from
their owner: the tick gate from 16-testing-ci.md §2.9, the three release
gates from §2.10.

```markdown
| # | DoD item / requirement | Evidence | Status |
|---|------------------------|----------|--------|
| 1 | R1 ≥60 fps playfield   | perf_frame.gate_render_frame_time + perf_tick.gate_tables, run <url>; overlay shot docs/audit/r1.png | PASS |
| 2 | R2 latency budgets     | Latency.InputToTickUnder4ms (p99.9 < 4 ms over ≥ 10,000 scripted press edges); F3 overlay export docs/audit/r2.png | PASS |
| 3 | R3 neon look + particles | 13-art-direction.md "Style checklist" ticked in M13 PR <url>; perf_particles.two_thousand_live_at_60fps; docs/audit/m13/*.png | PASS |
| 4 | Startup / load budgets | perf_startup.cold_boot_to_attract; perf_load.table_under_2s, run <url> | PASS |
| … | every PLAN.md §8 box and R1–R10 …                                            |
```

**Tasks.** 1) run the full suite + tools on all tables on all 3 OS via CI;
2) execute each PLAN.md §8 checkbox manually where it demands it (cabinet
display cases, packaged boot, author-a-table-from-the-guide spot check);
3) write the audit doc; 4) fix failures; 5) version/CHANGELOG/tag.

**Tests.** No new suites; the audit references existing green ones. One new
test: `Version.Is100` (version string matches the tag).

**Acceptance criteria.**

- [ ] Every PLAN.md §8 checkbox PASS with evidence (R1–R10).
- [ ] `docs/RELEASE-1.0-audit.md` committed; no row reads FAIL or blank.
- [ ] `v1.0.0` tagged; release artifacts published by CI.
- [ ] Final JOURNAL entry written; CHANGELOG 1.0.0 section complete.

**Demo artifact.** The audit evidence table itself, pasted in the PR body.
**Size.** S (~400 LOC, mostly docs).
**Spec refs.** PLAN.md §8, 01-product.md (R→evidence map), 16-testing-ci.md.

## Common pitfalls

- **Starting a milestone without reading its Spec refs.** The scope lines
  here are summaries; constants, schemas, and formulas live in the spec docs.
  Implementing from this file alone produces wrong numbers.
- **Committing a CI workflow without the files it reads on that same
  commit.** M0's `build-test.yml` begins every job that configures or builds
  — `build-test`, `asan`, `perf-gates`, `weekly-deep`, `det-soak`, but not
  the checkout-and-clang-format-only `format` job — with
  `uses: ./.github/actions/tb-setup`, the `asan` preset points
  `LSAN_OPTIONS` at `tests/lsan.supp`, and CMake reads
  `tests/quarantine.txt` at configure time. Any one of them missing turns M0
  red before a single line of C++ compiles, and the failure looks like a
  toolchain problem rather than a missing file.
- **Reaching outside the repo for SHA-256 at M0.** The font test hashes the
  `.ttf` bytes with the vendored `tests/third_party/picosha2.h`. Shelling out
  to `sha256sum` fails on `windows-latest` (no such tool), adding a crypto
  port changes `vcpkg.json` away from PLAN.md §5.2, and hand-rolling a digest
  inside an S-sized milestone is how the assertion quietly degrades into a
  file-size check that catches nothing.
- **Restating another document's owned type in this file.** 06-rendering.md
  §2 owns `IRenderer`, and it is the only renderer interface — an
  immediate-mode `class Renderer` alongside it is a defect. The same rule
  covers `Ball` (08 §1.2), the `tb_autoplay` CLI (14 §8.2), the FT bands
  (08 §5.7) and ramp seam layers (08 §6.10.2/§6.10.6, 09 §4.21, which are
  derived — an `exit_layer` field exists in no schema, no loader and no
  struct, and writing one is unknown key V026): this file wires milestones
  together, it never re-declares an interface. Two declarations means one is
  already wrong.
- **Turning an FT scenario into a replay tape.** FT-01…FT-10 run on the
  08-physics.md §5.6 rig — built in test code, no `table.json`, no
  `.tbreplay`, seed `0x54425354`, state-triggered input. There is no
  `tests/data/replays/` directory anywhere in this repo; test fixtures live
  under the 16-testing-ci.md §2 layout.
- **Registering a per-table tool test before the tool exists.** Tables ship
  from M5; `tb_validate`/`tb_autoplay` are stubs until M15. The per-table
  CTest loop lives inside `if(TB_TOOLS_READY)` (OFF until the M15 PR flips
  it) precisely so a perfectly good table never reddens CI on a stub's exit
  code.
- **Gating on a budget no document defines, or on a gate id nobody
  registers.** A perf box cites a registered gate id and nothing else: never
  a prose budget ("table load < 2 s per 09-table-format.md", "cold start
  under budget per 16-testing-ci.md" — neither document defines one), and
  never a plausible-looking abbreviation of a real id. The gate ids are
  exactly these seven; anything shorter or tidier is invented. The real ids
  are `perf_tick.gate_synthetic` and
  `perf_tick.gate_tables` (16-testing-ci.md §2.9),
  `perf_startup.cold_boot_to_attract`, `perf_frame.gate_render_frame_time`
  and `perf_load.table_under_2s` (§2.10), plus
  `perf_particles.two_thousand_live_at_60fps` (M13) and
  `perf_latency.input_to_tick_p999` (M4) — both also listed in
  16-testing-ci.md §2.9, which owns every one of these ids. Every perf
  acceptance box names one of those verbatim and quotes no number this file
  owns.
- **Gating a CPU budget with a frame period.** The M13 particle gate is the
  06-rendering.md §17.1 CPU-encode budget — 1.5 ms per frame, 9.0 % of a
  16.67 ms refresh — not the refresh itself. Swapping the two makes the gate
  ~11× looser and it then passes on any hardware, forever.
- **Reordering milestones or working ahead.** M6 code that "prepares" M9
  scripting is scope creep; the event payloads are already specified so no
  preparation is needed. One milestone in flight, always.
- **Hardcoding table content in C++.** From M5 on, all table-specific
  behavior lives in the table pack (canon §5.5). If a launch-table mechanic
  seems to need engine code, that is either a missing generic element feature
  (spec-change protocol) or a rules.lua job — never a `if (slug ==
  "neon-drift")`.
- **Letting `tb_sim` grow dependencies.** The `Layout.SimIncludesNothingForbidden`
  test exists because this happens gradually — an SDL type in a sim header,
  a render enum in an event. `tb_sim` links `tb_core` only (canon §5.1).
- **Treating skipped GPU tests as passing coverage.** CI runners usually lack
  GPUs; render smoke tests skipping is the documented fallback, not evidence.
  Visual acceptance criteria are verified on the dev machine and evidenced by
  committed screenshots in the PR.
- **Publishing renderer state from the sim thread or vice versa.** All
  cross-thread traffic goes through `SnapshotBuffer` and `EventRing`.
  Reaching across (e.g. render reading `SimState` directly) works until it
  corrupts a replay.
- **Sizing pools per frame instead of at table build.** Particle bursts,
  event rings, voices, and TOI iteration buffers are sized in
  `build_sim`/load. A `std::vector::push_back` in a tick is a red
  allocation-hook test.
- **Quantizing sounds or scripts to render frames.** Sounds schedule from
  sim ticks (M11), scripts run at tick granularity on the sim thread (M9).
  Anything hooked to the render loop drifts with refresh rate and breaks
  determinism.
- **Skipping the M16 guide-following requirement.** Authoring Atomic Diner
  from 15-launch-tables.md directly, ignoring 14-authoring-guide.md, defeats
  the dogfood: the deliverable is the table *and* the verified guide.
- **"Temporary" quality reductions to pass perf gates.** Disabling bloom in
  the perf test scene or shrinking the particle cap is gate-weakening
  (forbidden — 03-process.md §3.2). Profile, then fix the actual cost.
- **Forgetting the split rules on big milestones.** M13 and M17 are
  pre-split; anything else projecting past ~3,000 added lines must be split
  at a planned interface boundary *before* starting (03-process.md §5).

## Done when

- [ ] All 21 milestones (M0–M20, with authorized splits) merged in order,
      each via the full 03-process.md review loop.
- [ ] Every milestone's acceptance-criteria boxes were checked in its PR,
      with the named demo artifact attached.
- [ ] The continuously-enforced suites (global rule 4) were introduced at the
      stated milestones and never removed or weakened thereafter.
- [ ] `TB_TOOLS_READY` was OFF from M0 through M14 and flipped ON in the M15
      PR; no per-table `tb_validate`/`tb_autoplay` CTest entry was registered
      before the tools were real (D15).
- [ ] The perf ladder was climbed in order: `perf_tick.gate_synthetic` (M2),
      `perf_tick.gate_tables` (M5, `rules.lua` from M9),
      `perf_latency.input_to_tick_p999` (M4),
      `perf_particles.two_thousand_live_at_60fps` (M13, gated on the
      06-rendering.md §17.1 CPU-encode budget of 1.5 ms), and the M19 release
      gates `perf_startup.cold_boot_to_attract`,
      `perf_frame.gate_render_frame_time`, `perf_load.table_under_2s` — every
      id and threshold read from 16-testing-ci.md §2.9 (tick, latency,
      particles) and §2.10 (release), none invented here.
- [ ] Every test named in this document exists under `/tests` and is green on
      main, spelled under **16-testing-ci.md §2's taxonomy** — which owns the
      naming convention; the PascalCase labels here name required coverage,
      not gtest ids (global rule 7) — starting with M0's
      `unit_scaffold.sanity`, quoted verbatim from there.
- [ ] The repository layout, targets, and dependency rule match PLAN.md §5.1
      at every merge point.
- [ ] From M0 onward the repo contains everything CI reads —
      `.github/actions/tb-setup/action.yml`, `tests/lsan.supp`,
      `tests/quarantine.txt` — and the three vendored OFL faces with their
      licenses, `assets/fonts/SOURCES.md` and `assets/fonts/SHA256SUMS`
      under `/assets/fonts/`, hashed in-process against `SHA256SUMS` by
      `FontAssets.VendoredFontsPresentAndParse` using the vendored
      `tests/third_party/picosha2.h` (no crypto port, no `sha256sum`
      subprocess); `main` branch
      protection is configured per 03-process.md, or its "no admin rights"
      fallback is recorded in JOURNAL.md.
- [ ] All five tables + test-lab exist as pure text packs passing
      `tb_validate` and `tb_autoplay` (R5, R9).
- [ ] `docs/RELEASE-1.0-audit.md` shows PASS with evidence for every PLAN.md
      §8 item and every requirement R1–R10.
- [ ] `v1.0.0` is tagged and CI-built artifacts for Windows, Linux, and macOS
      exist and smoke-boot.
