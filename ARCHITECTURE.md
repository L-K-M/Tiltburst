# Architecture — Cross-Platform Pinball

**Status:** Original technical baseline — superseded in places (see banner)
**Last updated:** 2026-08-16

> **Read this first.** This is Tiltburst's original technical baseline. Its
> reasoning is still valuable and ADR-001–ADR-004 are kept intact, but it is
> **not** the final word on the stack. [PLAN.md](PLAN.md) §5 is canon, and
> [docs/plan/02-decisions.md](docs/plan/02-decisions.md) carries ADR-005+
> together with an authoritative table of every statement below that has been
> amended. **Wherever this document and that amendment table disagree, the
> table wins** (canon §5.10). Amended in places you will notice quickly: the
> rendering API (§2, §5 — SDL3 GPU API, no MoltenVK), the rendering approach
> (§5 — a 2-D instanced SDF vector renderer with no MSAA, cubemaps, or baked
> lighting), the goals table (§1), the repo layout (§9), the milestone list
> (§10 — see PLAN.md §6, M0–M20), and all three §12 open questions, which are
> resolved. Statements not listed in the amendment table remain in force.

This document records the initial technology choices and the reasoning behind
them. It is meant to be argued with. Where a decision is reversible, that is
noted; where it is load-bearing, that is noted too.

---

## 1. Goals

| Goal | Target |
|---|---|
| Playfield frame rate | Native refresh, no dropped frames (120 Hz+ where available) |
| Motion-to-photon latency | < 25 ms end to end |
| Flipper input → simulated response | < 4 ms |
| Displays | 2–3 concurrent (playfield, backglass, DMD/topper) |
| Platforms | Windows, Linux, macOS |
| Ball speed handled without tunnelling | 8 m/s against 2 mm geometry |

### Non-goals (for v1)

- VR / stereoscopic output
- Networked multiplayer
- Real-cabinet hardware I/O (solenoids, plunger encoders) — designed for, not built
- Mobile

---

## 2. Stack at a glance

| Layer | Choice | Alternative if it fails |
|---|---|---|
| Language | C++20 | Rust (see ADR-001 notes) |
| Window / input / audio device | SDL3 | GLFW + RtAudio |
| Rendering API | Vulkan 1.2, MoltenVK on macOS | SDL3 GPU API |
| Shader toolchain | HLSL → DXC → SPIR-V | GLSL → glslang |
| Physics | Custom, analytic primitives | Jolt (still needs custom flippers) |
| Audio mixing | miniaudio | SDL3 audio streams |
| Build | CMake 3.28+ | — |
| Dependencies | vcpkg manifest mode | git submodules |
| Table data | Custom JSON/binary format | — |

---

## 3. Latency and frame budget

This is the constraint that drives every other decision, so it goes before the
technology arguments.

At 120 Hz the frame budget is 8.33 ms. The chain we control:

```
input sample → physics tick → simulation state → record commands
   → GPU execution → present → scanout
```

Rules we hold ourselves to:

1. **Sample input at physics rate, not render rate.** Raw Input (Windows),
   evdev (Linux), IOKit HID (macOS). Never the windowing system's event queue
   for flipper buttons — SDL3 events are fine for menus.
2. **Late-latch.** Read the newest input immediately before the tick that
   consumes it, not at the top of the frame.
3. **Cap frames-in-flight at 1** on the playfield swapchain. Two frames of
   queued work is roughly a free 8 ms of added latency.
4. **Borderless fullscreen everywhere.** Exclusive fullscreen on the playfield
   will disturb or blank the other displays on at least one platform.
5. **No TAA, no frame generation, no render-thread deferral** of the playfield.
6. **Rotate the portrait playfield in the projection matrix**, never via OS
   display rotation — OS rotation costs a compositor pass on some drivers.

Instrument all of this from day one: a ring buffer of per-stage timings and an
on-screen overlay. Latency regressions are invisible until they are shipped.

---

## 4. ADR-001: Custom engine rather than Unity, Unreal, or Godot

**Status:** Accepted

### Context

Pinball is a narrow, demanding case: one to three dynamic bodies, a few hundred
static colliders, a very high physics rate, several displays, and a feel
requirement that is almost entirely a physics-tuning problem.

### Options considered

| Dimension | Unity | Unreal | Godot 4 | Custom C++ |
|---|---|---|---|---|
| Multi-display | Poor | Workable (nDisplay), heavy | Fair | Full control |
| Frame pacing control | Limited | Limited | Limited | Full control |
| Physics suitability | Poor | Poor | Poor | Built for purpose |
| Time to first prototype | Days | Days | Days | Weeks |
| Long-term ceiling | Low | Medium | Medium | High |

All three engines fail on the same two axes: multi-display support ranges from
afterthought to heavyweight, and none expose the present-mode and
frames-in-flight control the latency budget requires. Their physics engines are
tuned for many bodies at 60 Hz, which is the opposite of this workload.

### Decision

Custom C++ engine on SDL3.

### Consequences

- **Easier:** every latency and pacing decision above; physics tuned exactly
  for one ball; deterministic replays for free.
- **Harder:** no editor. Budget real time for a table-authoring tool — this is
  the most commonly underestimated cost of this decision.
- **Revisit if:** the team is small and content authoring becomes the
  bottleneck rather than simulation quality.

### Note on Rust

Rust is a defensible alternative and the borrow checker is genuinely useful for
a multi-threaded render/sim split. C++ wins here only on ecosystem gravity:
Visual Pinball X, PinMAME, and most reference material are C/C++, and reading
them is a large part of getting the feel right. If nobody on the team has that
attachment, Rust + `ash` + `winit` is a fine substitution and nothing else in
this document changes.

---

## 5. ADR-002: Vulkan with MoltenVK

**Status:** Accepted at the time — **superseded by ADR-005**
(docs/plan/02-decisions.md): v1 renders through the SDL3 GPU API, and the
rendering-approach paragraph below is amended there row by row.

Vulkan gives explicit swapchain configuration, present mode selection, and
control over queue submission — all three are load-bearing for section 3.
MoltenVK covers macOS at a modest cost; the alternative is a second Metal
backend, which is not worth it for the visual complexity of a pinball table.

**Fallback:** SDL3's GPU API abstracts all three native APIs and would speed up
early iteration considerably. It gives up some present-mode control. Reasonable
choice if Vulkan setup is eating the schedule — reversible with a week of work
if the renderer is kept behind an interface, which it should be regardless.

**Rendering approach:** forward rendering with MSAA. Deferred + TAA is the
modern default and it is wrong here: TAA smears a fast-moving ball, and motion
clarity of the ball *is* the game. Dynamic cubemap on the ball, updated at
reduced rate. Baked lighting for the playfield, dynamic only for inserts and
flashers.

---

## 6. ADR-003: Custom physics

**Status:** Accepted

### Context

A ball at 8 m/s moves 4 mm per millisecond. At a 60 Hz discrete-step
integration it moves 133 mm per step — straight through walls, flippers, and
targets. Every general-purpose engine's default configuration fails this.

### Decision

Custom solver:

- **Fixed timestep, 1000–2000 Hz.** With ≤3 balls and a few hundred static
  colliders this is cheap — single-digit microseconds per tick. Decouple from
  render rate entirely.
- **Continuous collision detection**, sphere against analytic primitives:
  line segments, arcs, planes, capped cylinders. No triangle soup on the hot
  path; use analytic shapes for collision and meshes only for display.
- **Broadphase:** uniform grid over the playfield. A table is ~50 × 100 cm; a
  fixed grid is simpler and faster than a BVH at this scale.
- **Flippers as bespoke constraints**, not generic hinge joints. Flipper feel —
  the ability to catch, cradle, backhand, and post-pass — comes from the
  angular velocity transfer model and is the single highest-value tuning
  surface in the project.

### Consequences

- Determinism is achievable and worth protecting: fixed timestep, no
  frame-rate-dependent terms, careful float discipline. It buys replays,
  regression tests, and reproducible bug reports.
- **Read Visual Pinball X's collision code before writing this.** It is open
  source and it is the reference standard for feel. Understanding why it does
  what it does will save months.

---

## 7. ADR-004: Multi-display architecture

**Status:** Accepted

### The failure mode to avoid

Driving playfield, backglass, and DMD from one render loop stalls the whole
frame on the slowest display's present. A 60 Hz backglass will pin a 144 Hz
playfield to 60.

### Decision

Independent swapchains, independent pacing:

| Window | Rate | Frames in flight | Notes |
|---|---|---|---|
| Playfield | Native refresh | 1 | The hot loop. Owns the frame budget. |
| Backglass | 30–60 Hz | 2 | Separate thread, own submission. |
| DMD / topper | 30 Hz | 2 | Cheap; can share the backglass thread. |

Simulation state is published once per physics tick into a triple-buffered
snapshot. Auxiliary windows read the latest snapshot; they never block the sim
and never block each other.

**Open question:** whether auxiliary windows get their own Vulkan queue or
share the graphics queue with explicit ordering. Queue availability varies by
vendor. Prototype both on AMD and NVIDIA before committing.

---

## 8. Audio

miniaudio, single header, all three platforms, small buffers. Flipper clack and
ball-hit latency is *felt* — target under 10 ms output latency and treat it as
part of the input chain, not as a separate subsystem.

Trigger sounds from the physics tick, not the render frame, or fast events get
quantised to frame boundaries and the table sounds mushy.

---

## 9. Proposed repository layout

```
/src
  /core        # allocators, math, job system, timing
  /platform    # SDL3 wrapper, raw HID input, file I/O
  /sim         # physics, collision, flipper model, table logic
  /render      # Vulkan backend, render graph, materials
  /audio       # miniaudio wrapper, event triggers
  /table       # table format loading, scripting hooks
  /tools       # table editor, asset pipeline
/assets
/shaders       # HLSL, compiled to SPIR-V at build time
/tests         # sim determinism + regression tests
/docs
```

Keep `sim` free of dependencies on `render` and `platform`. It should be
runnable headless — that is what makes deterministic regression testing
possible.

---

## 10. Milestones

1. **Grey box.** SDL3 window, Vulkan triangle, ball rolling on a flat plane
   with gravity and one wall. Prove the fixed-timestep loop.
2. **Flippers.** One flipper, tuned until catching a ball feels right. This
   milestone takes far longer than it looks and gates everything after it.
3. **Latency instrumentation.** The overlay from section 3. Do this before
   optimising anything.
4. **Full table geometry.** Slingshots, bumpers, targets, ramps, drains.
5. **Multi-display.** Second window, independent pacing, verify the playfield
   holds native refresh.
6. **Table format + editor.** The point at which content stops being hardcoded.

---

## 11. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Flipper feel never reaches "right" | High | Milestone 2 gates the project; study VPX physics early |
| Table editor cost underestimated | High | Scope it explicitly; consider Blender export as v1 |
| MoltenVK behavioural differences | Medium | CI on macOS from milestone 1, not later |
| Determinism lost to float drift | Medium | Regression tests with recorded input from milestone 1 |
| Vulkan setup consumes the schedule | Medium | Keep renderer behind an interface; SDL3 GPU as escape hatch |

---

## 12. Open questions (all three resolved)

The questions are kept for the reasoning that produced them; each is answered
in docs/plan/02-decisions.md and none is open.

- Table scripting: embedded Lua, or compiled C++ table modules? Lua is friendlier
  for modders; C++ avoids a whole class of frame-time surprises.
  → **Resolved (ADR-006):** embedded Lua 5.4 via sol2, sandboxed and
  deterministic, one `lua_State` per table on the sim thread; the frame-time
  worry is answered by a per-tick VM-instruction watchdog, not by a compiler.
- Original tables only, or support for existing table formats? The latter raises
  licensing questions that are much cheaper to answer now than after launch.
  → **Resolved (ADR-007):** original tables only. No VPX/FP/ROM/PinMAME
  importer in v1; VPX stays a physics-feel reading reference, never a source
  of copied code or converted content.
- Cabinet hardware support in scope for v2? If yes, the input abstraction needs
  to accommodate it from the start.
  → **Resolved (ADR-014):** out of scope for v1, designed for anyway — the
  input layer maps sources to actions with room for an analog axis, and the
  `SimEvent` ring is the feed a future solenoid/LED driver consumes.

---

## 13. References

- **Visual Pinball X** — open source, the reference for pinball physics feel.
  Windows/DirectX only, which is the gap this project fills.
- **libpinmame** — original table ROM emulation. Relevant only if supporting
  real table rules; carries IP considerations worth resolving early.
- SDL3, Vulkan, MoltenVK, Jolt Physics, and miniaudio documentation.
