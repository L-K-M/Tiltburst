# 02 — Architecture Decision Records (ADR-005+)

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 01-product.md, ../../ARCHITECTURE.md (which this document amends).

ARCHITECTURE.md carries ADR-001–ADR-004. This document adds ADR-005–ADR-014
and closes with the authoritative table of every ARCHITECTURE.md statement
they amend. Precedence: PLAN.md §5 canon > spec docs 01–16 > ARCHITECTURE.md
as amended here (canon §5.10). New ADRs are appended as ADR-015+ (PLAN.md §2.4).

## ADR-005 — SDL3 GPU API instead of raw Vulkan for v1

**Status:** Accepted. Amends ARCHITECTURE.md §2, supersedes §5 (ADR-002).

### Context

ARCHITECTURE.md ADR-002 chose Vulkan + MoltenVK for three load-bearing §3
controls: present-mode selection, frames-in-flight, submission control.
SDL3's GPU API (stable since SDL 3.2.0) exposes all three directly; raw
Vulkan is thousands of lines of ceremony with a large silent-failure surface
— the wrong trade for an autonomous LLM implementor (R10).

### Decision

Render with the SDL3 GPU API on all platforms; backends are D3D12 (Windows),
Vulkan (Linux), Metal (macOS) — no MoltenVK. The renderer sits behind
`tb::IRenderer` (06-rendering.md) with one v1 implementation,
`tb::GpuRendererSDL`; raw Vulkan remains a drop-in later. Every
ARCHITECTURE.md §3 rule maps to a concrete SDL3 GPU mechanism:

| ARCHITECTURE.md §3 rule | SDL3 GPU equivalent |
|---|---|
| 1. Sample input at physics rate | Unaffected by renderer; raw-input thread per canon §5.4 |
| 2. Late-latch input before the consuming tick | Unaffected; sim-thread behavior (05-engine-core.md) |
| 3. Frames-in-flight 1 on the playfield | `SDL_SetGPUAllowedFramesInFlight(device, 1)` at init (SDL default is 2 — must be set explicitly) |
| 3a. Present-mode control | `SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)`; `mode` = `SDL_GPU_PRESENTMODE_MAILBOX` if `SDL_WindowSupportsGPUPresentMode` allows, else `SDL_GPU_PRESENTMODE_VSYNC`; `IMMEDIATE` behind a debug setting for latency measurement |
| 3b. Backglass never stalls playfield | Playfield: `SDL_WaitAndAcquireGPUSwapchainTexture`; backglass: non-blocking `SDL_AcquireGPUSwapchainTexture`, **skip the frame** when the texture is NULL (canon §5.4) |
| 4. Borderless fullscreen | `SDL_SetWindowFullscreenMode(window, NULL)` + `SDL_SetWindowFullscreen(window, true)` |
| 5. No TAA / frame generation / render-thread deferral | Single-sampled RGBA16F offscreen `scene_color`, blitted through the post chain to the swapchain; **no MSAA** — SDF edges are analytically antialiased (`fwidth`-based AA width, 06-rendering.md §1, §8.3); no temporal passes |
| 6. Portrait rotation in projection | Projection-matrix rotation; never OS display rotation (canon §5.9) |

`allowed_frames_in_flight` is per-device, not per-window; Tiltburst sets 1
globally. The backglass tolerates this (~30 Hz, skip-on-unavailable);
ARCHITECTURE.md §7's "frames in flight: 2" for auxiliary windows is amended.

Row 5 also amends ARCHITECTURE.md §5's "**Rendering approach:** forward
rendering with MSAA" sentence: rendering stays forward, but MSAA is a v1
non-goal (06-rendering.md §1). Every render target in the chain is
single-sampled RGBA16F; no `SDL_GPU_SAMPLECOUNT_4` target and no resolve
step exists anywhere in `tb_render`. Antialiasing is analytic in the SDF
fragment shaders, which is both cheaper and sharper for the glow-heavy neon
look, and it is why the ball stays crisp at 12 m/s.

The rest of that §5 paragraph goes with it, and this is the largest single
departure from ARCHITECTURE.md: v1 is a **2-D instanced SDF vector renderer**
(06-rendering.md §1) with no 3-D meshes and no shadows. The ball's "dynamic
cubemap" becomes a procedural fake-chrome gradient computed in the fragment
shader (06-rendering.md §11), and "baked lighting for the playfield" becomes
no lighting bake at all — inserts and flashers are per-instance fill and glow
multipliers over emissive vector art (13-art-direction.md).

### Consequences

No MoltenVK; §7's Vulkan-queue open question is moot — all GPU
recording/submission stays on the main thread (canon §5.4). Shaders must
ship as SPIR-V + DXIL + MSL (ADR-012). Lost: explicit queues, timeline
semaphores, `VK_KHR_present_wait` — none needed for R1/R2 at 60–240 Hz.
`tb_render` never assumes a backend; no backend `#ifdef`s.

### Revisit if

M19 shows the swapchain path costing > 4 ms over theoretical minimum, or a
needed feature (HDR composition, present-timing feedback) is missing. Cost
is bounded by `tb::IRenderer`: one new implementation.

## ADR-006 — Lua 5.4 + sol2, sandboxed and deterministic

**Status:** Accepted. Resolves ARCHITECTURE.md §12 open question 1.

### Context

ARCHITECTURE.md left scripting open between embedded Lua and compiled C++
modules, fearing frame-time surprises. R9 requires plain-text tables;
compiled rules would put binaries in packs and a toolchain in the loop.

### Decision

Lua 5.4 embedded via sol2, one `lua_State` per loaded table, running on the
sim thread at tick granularity. Sandbox and API in 10-scripting.md (surface
is canon §5.7). Frame-time risk is handled by an instruction-count watchdog
(10-scripting.md §2.4 is normative): a **shared budget of 10,000 VM
instructions per tick**, not per handler, reset at the start of each tick's
script step and enforced with `lua_sethook(L, hook, LUA_MASKCOUNT, 1000)` —
the hook fires every 1,000 instructions, decrements the tick budget, and at
zero raises the Lua error `"instruction budget exceeded"`. The hook is
installed on the main state and on every coroutine. On overrun the running
handler/callback is aborted, **permanently disabled for the rest of the
game** (a disabled timer callback also cancels its timer), the remaining
handler invocations for that tick are skipped, and the game continues with
the budget refilled next tick. Disabling is deterministic: same inputs →
same overrun at the same tick. `tb_validate`/`tb_autoplay` treat an overrun
as a hard failure. The budget is the latency bound expressed in instructions:
10,000 is ≈ 100 µs at Lua 5.4's ~100M instr/s ceiling, which is exactly the
< 100 µs per tick the revisit criterion below allows scripting to take.

### Consequences

- LLMs and modders write Lua well; no compiler in the authoring loop.
- Determinism lands on the embedding: no `io`/`os`/`require`/`load`,
  `math.random` replaced by `tb.rng` (PCG32), and Lua built with a **fixed
  string-hash seed** (compile-time `luai_makeseed` override) — stock Lua
  randomizes it per process, changing `pairs` iteration order and silently
  breaking replays. Build flags and the test: 10-scripting.md.
- vcpkg ports `lua` (pinned 5.4.x), `sol2`; `SOL_ALL_SAFETIES_ON` in debug.
- Watchdog tuning: if a legitimate tick exceeds the shared budget, profile
  first; raise only if that tick's scripting still completes in < 100 µs on
  Profile A. Headroom is about 3× — a full event burst in the shipped tables
  measures under 3,000 instructions against the 10,000 budget
  (10-scripting.md §2.4). Acceptance: all shipped tables run `tb_autoplay`
  with zero watchdog aborts.

### Revisit if

A shipped table needs > 100 µs/tick of scripting sustained, or the sandbox
proves escapable (audit in M9).

## ADR-007 — Original tables only; no import of VPX or other formats

**Status:** Accepted. Resolves ARCHITECTURE.md §12 open question 2.

### Context

Existing formats (VPX, FP) overwhelmingly recreate commercial machines whose
art, sound, and rules are third-party IP; an importer is an IP liability and
a compatibility tar pit (VPX rules are VBScript against a different engine).

### Decision

Tiltburst loads only its own pack format (canon §5.5). No VPX/FP/ROM/PinMAME
support in v1; ARCHITECTURE.md §13's libpinmame reference is background
only. VPX **source code** stays a physics feel reference (ARCHITECTURE.md
ADR-003): reading is encouraged, but no code is copied (keeps Tiltburst's
license clean of GPL obligations) and no content is converted.

### Consequences

Clean IP story; the five shipped tables are original (canon §5.8); no
compatibility constraints leak into 09-table-format.md. Community content
requires authoring, not converting — mitigated by R9 tooling.

### Revisit if

A v2 converter would be a separate offline tool with its own legal review —
never a runtime loader.

## ADR-008 — All table content is plain text

**Status:** Accepted. Amends ARCHITECTURE.md §2 table-data row; re-scopes
ADR-001's "table editor" consequence.

### Context

R9/R10: the content pipeline must be drivable by an LLM through PRs. LLMs
cannot emit PNGs or WAVs reliably; binaries cannot be reviewed in diffs; a
GUI editor (ARCHITECTURE.md §11's high-risk item) is exactly what an
autonomous implementor builds and tests worst.

### Decision

A table pack is four text files — `table.json`, `rules.lua`, `art.json`
(TBArt vector art), `audio.json` (synth patches + tracker music) — per canon
§5.5. Binary assets (PNG/WAV in `assets/`) are an optional convenience for
human authors; no shipped table uses them and `tb_validate` warns on their
presence. No GUI editor: the authoring loop is a text editor + `tb_validate`
+ `tb_autoplay` + `tb_screenshot` (14-authoring-guide.md).

### Consequences

Table packs diff, review, and merge like code, so the review loop covers
content PRs. 12-audio.md and 13-art-direction.md must make synthesized and
vector content look and sound shipped (pillar 3). The editor-cost risk
converts to tooling + guide cost, owned by M15/M16 (R9.2, R9.3).

### Revisit if

M16 dogfooding shows text-only authoring cannot reach shippable art quality;
the fallback is richer TBArt prefabs, never a binary format.

## ADR-009 — 2.5D physics: ramps as 1-D paths, layers as 2-D planes

**Status:** Accepted. Details in 08-physics.md; schema in 09-table-format.md.

### Context

Full 3D rigid-body simulation multiplies solver complexity, CCD cases, and
authoring difficulty, against R7 and R10. The ball spends ~95 % of its time
on a plane.

### Decision

The simulation is layered 2-D ("2.5D"). Main playfield: 2-D plane
(`layer: 0`) with slope-as-gravity (canon §5.3). **Ramps are constrained 1-D
paths**: on a ramp the ball's state is (arc-length `s`, `ds/dt`) along a
polyline/arc path with height profile `h(s)`; gravity projects onto the path
tangent; entry via a capture window, exit at either end with reconstructed
2-D velocity; a reject is a path reversal (ball rolls back down), never a
sideways fall. **Upper playfields are free 2-D areas on `layer: 1`** with
their own colliders; layer transitions happen only via ramps, VUKs, and
drop-offs (deterministic ballistic arcs, no collision until touchdown).
Full 3D is rejected for v1: CCD stays sphere-vs-analytic-2-D primitives
(ARCHITECTURE.md ADR-003) and authors never write 3-D collision geometry.

### Consequences

Tunnelling analysis stays tractable at 1000 Hz and the 12 m/s clamp; a ramp
in `table.json` is a path + `h(s)`, not a mesh. Accepted losses: balls
hopping over posts, sideways ramp falls.

### Revisit if

Post-v1 only. Nothing in v1 may depend on true 3-D ball state.

## ADR-010 — Local multiplayer only in v1

**Status:** Accepted. Confirms ARCHITECTURE.md §1 non-goal.

### Context and decision

R6 requires 1–4 alternating players plus 2-player Duel — all hot-seat;
netcode would dilute the project's value (feel + authorable content). All
multiplayer is local: alternating 1–4 (01-product.md §5.1) and Duel
(01-product.md §5.2, spec in 11-game-framework.md). Networked play stays a
non-goal: no network code, protocol hooks, or lobby abstractions in v1. The
only seam kept is what determinism already requires — a serializable
input/event stream (canon §5.3). Nothing more is added.

### Consequences

Simpler state machine; the determinism suite doubles as a future netcode
foundation at zero extra cost.

### Revisit if

v2 planning, and only after R1–R10 are shipped.

## ADR-011 — 60 Hz reference hardware; "never below 60 fps, up to 240 Hz"

**Status:** Accepted. Amends ARCHITECTURE.md §1 goals table.

### Context

ARCHITECTURE.md targeted "120 Hz+" and motion-to-photon < 25 ms
unconditionally. The reference cabinet (01-product.md §2.1) has two 60 Hz
TVs; present-to-scanout alone can cost 16.7 ms there, so < 25 ms end-to-end
is physically unreachable on that hardware.

### Decision

Binding target: **the playfield never drops below 60 fps on Profile A, and
native refresh is supported up to 240 Hz** (R1). Latency splits by what
software controls: input → sim < 4 ms always, on all hardware, never relaxed
(R2.1 — refresh-independent via raw-input thread + 1000 Hz sim);
motion-to-photon < 25 ms on 120 Hz-class hardware (R2.2); at 60 Hz the
requirement becomes "no software-added latency" — frames-in-flight 1,
MAILBOX-else-VSYNC per ADR-005, software-estimated chain ≤ 40 ms (R2.3).

### Consequences

Perf gates (16-testing-ci.md) test 60 Hz lock and high-refresh scaling. Sim
rate (1000 Hz) and audio latency (< 10 ms) are refresh-independent — they
are why 60 Hz Tiltburst still *feels* fast.

### Revisit if

The reference cabinet gains a high-refresh playfield display; targets
tighten to the 120 Hz row with no code change expected.

## ADR-012 — HLSL via SDL_shadercross at build time; committed blobs fallback

**Status:** Accepted. Amends ARCHITECTURE.md §2 shader toolchain row.

### Context

ADR-005 requires SPIR-V (Vulkan) + DXIL (D3D12) + MSL (Metal) from one
source; ARCHITECTURE.md's "HLSL → DXC → SPIR-V" covers only Vulkan.
SDL_shadercross (libsdl-org) compiles HLSL to all three, offline.

### Decision

Shaders are authored once in HLSL under `/shaders`, compiled **at build
time** by the SDL_shadercross CLI (CMake FetchContent, canon §5.2) into
`/shaders/compiled/<name>.{spv,dxil,msl}` and loaded by backend format; no
runtime shader compilation in the shipped binary. Fallback: the compiled
blobs are **committed** under `/shaders/compiled`, refreshed in any PR
touching HLSL; CI recompiles and diffs them. CMake option
`TB_COMPILE_SHADERS` (default ON) toggles the compile step; OFF uses the
committed blobs verbatim — a missing toolchain degrades to "use blobs".

### Consequences

One shader dialect for the implementor. Committed blobs are a scoped
exception to ADR-008's text-only ethos: machine-generated, never
hand-edited, kept honest by the CI diff (stale blobs fail the PR).

### Revisit if

SDL_shadercross cannot express a needed feature; fallback is per-backend
source for that shader only, recorded as a new ADR.

## ADR-013 — GoogleTest; determinism is per-platform, not cross-platform

**Status:** Accepted. Sharpens ARCHITECTURE.md §6/§11 determinism claims.

### Context

Canon §5.3: same binary + same seed + same inputs ⇒ identical simulation.
Cross-platform bit-exactness would additionally require identical float
contraction, FMA, and libm behavior across MSVC/Clang/GCC — achievable only
with constraints (strict FP modes, software transcendentals) that tax the
1000 Hz budget and the implementor.

### Decision

Test framework: GoogleTest (vcpkg `gtest`), one `tb_tests` binary mirroring
`/src` (canon §5.1); sim tests run headless. Determinism contract:
**per-platform** — the suite records input streams and asserts bit-identical
`SimSnapshot` sequences on replay against platform-tagged goldens, on all
three OSes in CI. Cross-platform bit-exactness is **not required and not
tested**; cross-platform checks are statistical (autoplay score
distributions within bands, 16-testing-ci.md). Float discipline still
applies (fixed dt, no wall-clock reads, seeded PCG32, ordered iteration);
`-ffast-math` / MSVC `/fp:fast` is forbidden in `tb_sim`, precise-FP flags
pinned per-target in CMake.

### Consequences

Replays, regression tests, and reproducible bug reports work per platform
(where bugs are reproduced anyway). Replay files are platform-tagged;
tooling must refuse a foreign replay rather than desync silently.

### Revisit if

A post-v1 networked mode needs lockstep across platforms; the costed path is
fixed-point or software-float inside `tb_sim`.

## ADR-014 — Cabinet hardware I/O: designed for, not built

**Status:** Accepted. Resolves ARCHITECTURE.md §12 open question 3.

### Context

Cabinet conversions eventually want solenoid feedback, addressable LEDs, and
analog plunger encoders. ARCHITECTURE.md asked whether v2 hardware must
shape v1 abstractions.

### Decision

Out of scope for v1 (01-product.md §8), but abstractions must not preclude
it. **Input:** the input layer (05-engine-core.md) maps *sources* to
*actions*; a v1 source is a scancode, and the source enum leaves room for
`AnalogAxis` (plunger position 0–1) — the plunger sim already consumes a
normalized pull value, so an analog encoder later replaces only the value
producer. **Output:** the `SimEvent` ring buffer (canon §5.4) is the
canonical feed for any future feedback device — a solenoid driver becomes
one more consumer beside audio. No device code or protocols in v1.

### Consequences

Nearly zero v1 cost: one enum shape and one normalized plunger value. YAGNI
applies to everything else (no LED protocols, no speculative interfaces).

### Revisit if

v2 scopes cabinet I/O; start from the `SimEvent` consumer model.

## ADR-015 — Orbitron role filled by Chakra Petch Bold (font substitution)

**Status:** Accepted (M0). Applies 13-art-direction.md §5.1's own fallback
row and 03-process.md §3.2.

### Context

§5.1 pins `ofl/orbitron/Orbitron-Bold.ttf` (or a static instance under
`ofl/orbitron/static/`) from github.com/google/fonts. At the pinned commit
(`6a003b5eb672dc8bf5bff5937cf5863f8b175445`, fetched 2026-08-25) upstream
ships only the variable font `Orbitron[wght].ttf`; both static paths 404.
§5.1 simultaneously **forbids vendoring the variable font**: stb_truetype
has no variation-instancing, so it would bake at weight 400 and the HUD
would silently lose its Bold.

### Decision

The **Role/Rules columns of §5's font table stay binding; the family name
does not** (that is exactly the substitution clause §5.1 ends with). The
orbitron role — geometric square sans for HUD/score numerals — is filled by
**Chakra Petch Bold** (`ofl/chakrapetch/ChakraPetch-Bold.ttf`, OFL 1.1,
same pinned commit), vendored byte-exact as
`assets/fonts/ChakraPetch-Bold.ttf` with its license and a SHA256SUMS line.
The logical font name in code remains `orbitron`. Monoton and Righteous are
unaffected.

### Consequences

M13 consumes a real static Bold; no engine change. Typography deltas vs
true Orbitron (slightly narrower caps, humanist details) are acceptable for
the role and noted for the M13 style checklist. If upstream ever restores
static Orbitron, re-vendoring requires only SOURCES.md + SHA256SUMS updates
plus a JOURNAL entry — no code change.

## ADR-016 — Vendored third-party headers are exempt from the format check

**Status:** Accepted (M0). Amends 16-testing-ci.md §3.2's `format` job.

### Context

The `format` job as specified runs `find src tests -name '*.cpp' -o -name
'*.h'` over every header, which includes vendored third-party sources such
as `tests/third_party/picosha2.h`. Reformatting a vendored file breaks its
byte-exact provenance pin (SOURCES.md upstream commit), and excluding it by
hand-editing it into compliance is unmaintainable. As written, M0's format
check can never pass.

### Decision

The format job's `find` gains `-not -path 'tests/third_party/*'`: vendored
code under any `third_party/` directory is excluded from clang-format
enforcement everywhere, forever. First-party code under `/src` and `/tests`
is unaffected and remains fully enforced.

### Consequences

One-line workflow diff; no required-check name changes. Future vendored
assets must live under a `third_party/` directory to inherit the exemption.

## Amendments to ARCHITECTURE.md (authoritative table)

Where ARCHITECTURE.md disagrees with a row below, the amendment wins (canon
§5.10). Statements not listed remain in force.

| ARCH § | Original claim | Amendment | ADR |
|--------|----------------|-----------|-----|
| §1 | Playfield "native refresh … (120 Hz+ where available)" | Reference is 60 Hz; target "never below 60 fps, native refresh up to 240 Hz" | ADR-011 |
| §1 | "Motion-to-photon < 25 ms end to end" | < 25 ms at 120 Hz-class; ≤ 40 ms software-estimated at 60 Hz; input→sim < 4 ms unchanged | ADR-011 |
| §1 | Displays "2–3 concurrent (playfield, backglass, DMD/topper)" | v1 drives exactly 2 (playfield + backglass) with single-display fallback; DMD/topper deferred | ADR-005, 01 §8 |
| §2 | Rendering "Vulkan 1.2, MoltenVK on macOS" | SDL3 GPU API (D3D12/Vulkan/Metal); raw Vulkan is the deferred alternative behind `tb::IRenderer` | ADR-005 |
| §2 | Shaders "HLSL → DXC → SPIR-V" | HLSL → SDL_shadercross → SPIR-V + DXIL + MSL at build time; committed blobs fallback | ADR-012 |
| §2 | Table data "Custom JSON/binary format" | Plain-text-only pack (JSON + Lua + TBArt + audio JSON); optional binaries for humans only | ADR-008 |
| §4 (ADR-001) | "Budget real time for a table-authoring tool" (GUI editor) | No GUI editor; text authoring + tb_validate / tb_autoplay / tb_screenshot + guide | ADR-008 |
| §5 (ADR-002) | Vulkan accepted; SDL3 GPU is the fallback | Inverted: SDL3 GPU accepted for v1; raw Vulkan is the revisit path | ADR-005 |
| §5 (ADR-002) | Rendering approach "forward rendering with MSAA" | Forward stands; MSAA is a v1 non-goal — single-sampled RGBA16F targets, analytic SDF antialiasing (06-rendering.md §1) | ADR-005 |
| §5 (ADR-002) | "Dynamic cubemap on the ball, updated at reduced rate" | No cubemap and no reflection probe: the ball is a procedural fake-chrome gradient in the SDF fragment shader (06-rendering.md §11) — a 2-D renderer has no 3-D scene to reflect | ADR-005 |
| §5 (ADR-002) | "Baked lighting for the playfield, dynamic only for inserts and flashers" | No lightmaps, meshes, or shadows anywhere: the playfield is 2-D instanced SDF vector art lit emissively, and every insert/flasher is a per-instance fill+glow multiplier recomputed each frame (06-rendering.md §1, §14; 13-art-direction.md) | ADR-005 |
| §6 (ADR-003) | "Fixed timestep, 1000–2000 Hz" | Fixed 1000 Hz exactly, dt = 0.001 s (canon §5.3) | — (canon) |
| §6 (ADR-003) | "Determinism … careful float discipline" | Sharpened: per-platform bit-exact; cross-platform bit-exactness a non-goal; fast-math forbidden in `tb_sim` | ADR-013 |
| §7 (ADR-004) | Backglass "separate thread, own submission", frames in flight 2 | All GPU submission on the main thread; backglass ~30 Hz via non-blocking acquire, frame skipped if unavailable; device-wide frames-in-flight 1 (canon §5.4) | ADR-005 |
| §7 (ADR-004) | DMD/topper row (30 Hz, shares backglass thread) | Deleted for v1 | ADR-005, 01 §8 |
| §7 (ADR-004) | Open question: own Vulkan queue vs shared; "prototype on AMD and NVIDIA" | Moot under single-threaded SDL3 GPU submission; no prototyping task | ADR-005 |
| §9 | Repo layout incl. "job system", "table editor" in /tools, Vulkan in /render | Superseded by canon §5.1 (no job system; tools are the three CLIs; /render is SDL3 GPU) | ADR-005/008 |
| §10 | Milestones 1–6 ("Vulkan triangle", "editor") | Superseded by PLAN.md §6, M0–M20 | — (canon) |
| §11 | Risk "MoltenVK behavioural differences" | Moot; replaced by "SDL3 GPU backend variance", mitigated by 3-OS CI from M0 | ADR-005 |
| §11 | Risk "Table editor cost … consider Blender export" | Re-scoped to tooling + authoring-guide cost (M15/M16); no Blender export | ADR-008 |
| §12 | Open question: Lua vs C++ table modules | Resolved: Lua 5.4 + sol2, sandboxed, watchdogged | ADR-006 |
| §12 | Open question: original tables vs existing formats | Resolved: original only; no importers | ADR-007 |
| §12 | Open question: cabinet hardware in scope for v2? | Resolved: abstractions accommodate it; no v1 implementation | ADR-014 |

## Common pitfalls

- **Forgetting `SDL_SetGPUAllowedFramesInFlight(device, 1)`.** SDL defaults
  to 2; the budget silently gains ~a frame. Set at init, assert in a test.
- **Blocking acquire on the backglass.** `SDL_WaitAndAcquireGPUSwapchainTexture`
  there stalls the playfield at 60 Hz. Backglass uses non-blocking
  `SDL_AcquireGPUSwapchainTexture`, NULL ⇒ skip frame (R1.3 tests this).
- **Calling GPU functions off the main thread.** All recording/submission is
  main-thread (canon §5.4); a background upload breaks some backends.
- **Opening the full Lua stdlib then deleting pieces.** Build the sandbox
  additively: only pruned `base`, `table`, `string`, `math` (minus
  `random`/`randomseed`); never `luaL_openlibs` (10-scripting.md).
- **Shipping Lua with a randomized string-hash seed.** `pairs` order varies
  per process; replays desync intermittently. Pin the seed at build time.
- **Treating the instruction budget as per handler.** It is 10,000
  instructions **shared by every script invocation in one tick**, hooked
  every 1,000 (ADR-006); an overrun permanently disables the offending
  handler and skips the tick's remaining handlers. Sizing a handler against
  the whole budget will starve the ones registered after it.
- **Building 3-D because ARCHITECTURE.md §5 describes it.** No meshes,
  shadows, cubemap on the ball, or baked playfield lighting exist in v1 —
  the amendment table's §5 rows replace all four with 2-D SDF vector art,
  fake chrome, and per-instance light multipliers.
- **Adding an MSAA target "for quality".** Every render target is
  single-sampled RGBA16F; SDF edges are antialiased analytically (ADR-005
  row 5). A resolve step buys blur and bandwidth, not quality.
- **Writing cross-platform golden-hash tests.** They fail on the second OS.
  Goldens are platform-tagged (ADR-013); cross-platform checks statistical.
- **Enabling fast-math globally.** Forbidden in `tb_sim` (ADR-013); pin FP
  flags per-target in CMake, not per-project.
- **Runtime HLSL compilation, or hand-editing `/shaders/compiled`.** Shaders
  compile at build time; blobs are generated and CI-diffed (ADR-012).
- **Building speculative seams** — network lobbies, LED protocols, 3-D ball
  state. ADR-009/-010/-014 define the only permitted seams; reject the rest
  in review as scope creep.
- **Importing VPX code or content.** VPX is GPL reference reading only
  (ADR-007); no code copying, no format loaders.

## Done when

- [ ] `vcpkg.json` lists exactly the canon §5.2 ports; no Vulkan SDK,
      MoltenVK, or DXC dependency anywhere in the build.
- [ ] `tb_render` has a `tb::IRenderer` interface with a single SDL3 GPU
      implementation; `grep -ri vulkan src/` matches only comments.
- [ ] Device init sets frames-in-flight 1 and MAILBOX-else-VSYNC, asserted
      by a renderer init test; the R1.3 backglass-stall test passes.
- [ ] No multisampled target or resolve pass exists: `grep -r SAMPLECOUNT
      src/` matches nothing but `SDL_GPU_SAMPLECOUNT_1`; every offscreen
      target is RGBA16F (ADR-005 row 5, 06-rendering.md §7).
- [ ] `/shaders` holds HLSL; `/shaders/compiled` holds committed
      SPIR-V/DXIL/MSL; CI recompiles and diffs; `TB_COMPILE_SHADERS=OFF`
      builds green with the toolchain absent.
- [ ] Lua sandbox tests prove: no `io`/`os`/`require`/`load`; `math.random`
      absent; `tb.rng` deterministic; the watchdog aborts a spin loop at the
      shared 10,000-instruction tick budget, permanently disables that
      handler, skips the tick's remaining handlers, and refills next tick;
      `pairs` order stable across 1,000 runs.
- [ ] Determinism suite green on all three OSes with platform-tagged
      goldens; no test asserts cross-platform bit-exactness.
- [ ] No importer, network, solenoid, LED, or DMD code exists; the input
      layer exposes only the ADR-014 seams.
- [ ] Every amendment-table row is reflected in the codebase; later
      deviations are recorded as ADR-015+ here with a JOURNAL.md note.
