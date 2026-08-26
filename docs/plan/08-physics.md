# 08 — Physics

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 05-engine-core.md (fixed-timestep loop, input late-latch, PCG32),
09-table-format.md (element JSON parameters, geometry paths),
10-scripting.md (event delivery to Lua), 16-testing-ci.md (determinism and
perf gates).

This document is the complete specification of the Tiltburst simulation. The
implementor must be able to code the entire solver from this document alone.
Everything lives in `/src/sim` (`tb_sim`), which depends only on `tb_core`
and must build and run headless.

## 1. Model overview

### 1.1 The 2.5D model

Tiltburst simulates pinball as **2.5D**: the ball moves freely in 2-D on the
playfield plane (a "layer"), and moves in constrained 1-D along ramp paths.
There is no free 3-D flight. The z coordinate exists only as a derived value
(ramp height profile) used for rendering and for gravity along ramps.

```
        +y (up-table, away from player)
        ^
        |   layer 1 (upper playfield: free 2-D)
        |     ^
        |     | ramp (constrained 1-D path with height profile z(s))
        |     |
        |   layer 0 (main playfield: free 2-D)
        +---------> +x (right)
      origin: bottom-left of play area (canon §5.3)
```

A ball is always in exactly one of three motion modes:

| Mode | State | Integrated by |
|---|---|---|
| `FREE` | 2-D position/velocity on a layer | §2 tick, §3 CCD |
| `RAMP` | 1-D arc-length `s` on a ramp path | §6.10 |
| `CAPTURED` | held by kicker / ball_lock / trough | element timers, §6.9/§6.14/§6.15 |

### 1.2 Ball state

```cpp
struct Ball {
  // identity
  uint8_t  index;         // 0..kMaxBalls-1, stable while ball is live
  bool     live;          // false = slot free (ball in trough or removed)
  // FREE mode
  Vec2     pos;           // meters, table space
  Vec2     vel;           // m/s
  float    omega_z;       // rad/s, spin about the playfield normal (english)
  uint8_t  layer;         // 0 = main playfield, 1 = upper playfield
  Vec2     last_safe_pos; // last tick-end position with no penetration
  // RAMP mode
  BallMode mode;          // FREE | RAMP | CAPTURED
  uint16_t ramp_elem;     // element index when mode == RAMP
  float    s;             // arc length along ramp path, meters
  float    s_dot;         // ds/dt, m/s (signed; + = toward far end)
  // CAPTURED mode
  uint16_t holder_elem;   // capturing element index
  uint32_t hold_ticks;    // kicker: ticks to the capture_ms auto-eject
                          //   (0 = script-held via tb.kick_hold, §6.9);
                          // ball_lock: ticks to the §6.14 unclaimed
                          //   auto-release (0 = released or claimed).
                          // Neither countdown ever pauses — not on tilt.
};
```

`kMaxBalls = 6` — equal to the maximum trough `capacity`
(09-table-format.md §4.18), i.e. the most physical balls any table can have
in the machine at once (default 4 plus `tb.add_ball` headroom). This value
is authoritative: `SimSnapshot`'s `BallSnap` array (05-engine-core.md §7.1)
is sized by this same constant. Ball constants
(canon §5.3): radius `r = 0.0135 m`, mass `m = 0.08 kg`. Moment of inertia
(solid sphere): `I = (2/5)·m·r² = 5.832e-6 kg·m²`.

**Spin model (deliberate simplification, do not "improve"):** only spin about
the playfield normal (`omega_z`) is tracked. Rolling spin about in-plane axes
is abstracted into rolling resistance. `omega_z` has **no effect on free
rolling** — the spin axis passes through the playfield contact point, so it
produces no contact-point velocity and therefore no curving force. Do not add
Magnus lift or spin-curving on open playfield. `omega_z` matters only in
contacts (walls, flippers, other balls), where it feeds the friction impulse
(§4.1), which is how "english" changes bounce direction in real pinball.

### 1.3 Forces, integration, damping

Fixed tick `dt = 0.001 s` exactly (1000 Hz, canon §5.3). Integrator:
**semi-implicit Euler** — velocity first, then position:

```
v ← v + a·dt          (all accelerations)
v ← clamp_speed(v, 12.0)             // canon max ball speed
x ← x + v·dt          (via the CCD loop, §3.6 — never a bare add)
```

Accelerations applied every tick to every FREE ball, in this order:

1. **Slope gravity** (canon §5.3): `a_g = (0, −g·sin(slope))` with
   `g = 9.81 m/s²`, per-table `slope` (default `6.5°` →
   `a_g = 1.1105 m/s²`). Applies identically on all layers.
2. **Magnet forces** (§6.12), summed over active magnets on the ball's layer.
3. **Quadratic air drag:** `a_d = −k_drag·|v|·v`, `k_drag = 0.002 m⁻¹`
   (derived from ρ=1.2 kg/m³, C_d=0.47, A=πr², m=0.08 kg).
4. **Rolling resistance:** `a_rr = −μ_rr·g·cos(slope)·v̂` with
   `μ_rr = 0.025` (dimensionless; per-table override
   `physics.rolling_resistance`, **range 0.005–0.060**, declared in the
   optional top-level `"physics"` block of `table.json` —
   09-table-format.md §2 is the schema owner of that block, this section
   owns the meaning). At defaults `a_rr = 0.025·9.81·cos(6.5°) =
   0.2437 m/s²`. Apply only if `|v| > 0`; if the tick's
   `|a_rr|·dt ≥ |v|`, set `v = 0` instead (never reverse velocity).
5. **Nudge envelope** (§7), when active.

**Per-table `physics.*` overrides (binding).** Every `physics.*` name used
anywhere in this document denotes a key of that single optional `"physics"`
block, and **09-table-format.md §2 is its sole schema owner** (key set,
defaults, hard ranges, validation). 09 §2 declares exactly this authorable
set — this document owns what each key *means*, 09 §2 owns its range,
default and validation:

| `physics.*` key | Meaning (owned here) | Default |
|---|---|---|
| `rolling_resistance` | μ_rr in `a_rr = −μ_rr·g·cos(slope)·v̂` (item 4 above) | 0.025, range 0.005–0.060 |
| `restitution_falloff` | `kFalloff` in s/m — the §4.2 restitution-curve rolloff rate | 0.12 |
| `restitution_soft` | `kSoft` in m/s — approach speed below which `e` is full (§4.2) | 0.5 |
| `live_catch_window_ms` | §5.4 catch window measured from end-of-stroke, in ms (converted to ticks at load) | 50, range 30–80 |
| `live_catch_factor` | §5.4 multiplier applied to `e_eff` inside that window | 0.15, range 0.05–0.30 |
| `tilt.warn` | §7.2 bob warn threshold in m (`danger_threshold{BOB_WARN}`) | 0.055 |
| `tilt.hard` | §7.2 bob hard threshold in m (`danger_threshold{BOB_HARD}`) | 0.085 |
| `tilt.abuse` | §7.3 abuse-accumulator threshold in m/s (`danger_threshold{ABUSE}`) | 1.2 |

`tilt` is a sub-object; the block, the sub-object and every key are
optional, and omitting one uses the default above. A `physics.*` name not
in this table is *not* authorable: it stays a global constant, tunable only
in code, and 09 §2 flags it as an unknown key (V026). Adding a `physics.*`
override therefore means adding it here **and** to 09 §2 in the same PR —
never one without the other.

Spin damping (every tick, FREE balls): `omega_z ← omega_z · exp(−0.7·dt)`
(time constant ≈ 1.4 s). Clamp `omega_z` to ±220 rad/s after every change.

Speed clamp: after the force phase **and** after every contact impulse,
`if |v| > 12.0: v ← v·(12.0/|v|)`.

### 1.4 Global constants

| Name | Value | Meaning |
|---|---|---|
| `kTickDt` | 0.001 s | fixed tick |
| `kBallRadius` | 0.0135 m | ball radius |
| `kBallMass` | 0.08 kg | ball mass |
| `kBallInertia` | 5.832e-6 kg·m² | (2/5)mr² |
| `kMaxSpeed` | 12.0 m/s | velocity clamp |
| `kMaxSpin` | 220 rad/s | \|omega_z\| clamp |
| `kGravity` | 9.81 m/s² | g |
| `kDragK` | 0.002 m⁻¹ | quadratic drag |
| `kRollMu` | 0.025 | rolling resistance coefficient |
| `kSpinDamp` | 0.7 s⁻¹ | free spin decay rate |
| `kSkin` | 1e-4 m | contact separation kept after TOI |
| `kToiEps` | 1e-9 s | TOI tie/validity epsilon |
| `kRestSpeed` | 0.15 m/s | below this normal speed, restitution = 0 (ADR-021: raised from 0.05; the cutoff must exceed the micro-bounce approach band or restitution self-sustains a rattle) |
| `kMaxToiIter` | 8 | contact resolutions per ball per tick |
| `kGridCell` | 0.032 m | broadphase cell size |

## 2. The simulation tick

### 2.1 Tick order

The tick is a fixed pipeline. Never reorder; ordering is part of determinism.

The pipeline below is the inside of the binding **4-phase tick** stated
identically by 05-engine-core.md §6.3, 10-scripting.md §2.2 and
11-game-framework.md §1: steps 1–6 are **phase 1** (physics integration +
sim event generation) and step 7 runs **phases 2, 3 and 4**. All four
documents describe one pipeline; if they ever differ they are wrong, not
alternatives.

```
tick(n):
  1. LATCHED SCRIPT ACTIONS + LATE-LATCH INPUT
     First apply the physical script actions latched during tick n−1
     (`tb.kick`, `tb.kick_hold`, `tb.release_lock`, `tb.magnet_*`,
     `tb.set_flipper_enabled`, `tb.add_ball`, `tb.drop_bank_reset`,
     `tb.gate_*` — 10-scripting.md §2.2), so physics state is immutable
     while scripts run. Then read the raw-input atomic snapshot exactly
     once (05-engine-core.md).
     This is the only input read this tick. Derive logical button states:
     flipper_left/right/upper_*, launch, nudge_{left,right,up}, start.
     Generate this tick's **cabinet-button switches** from press edges only
     (§6 preamble, 10-scripting.md §4.1): flipper_left →
     `switch_hit{id="button_flipper_left", ball_id=0, speed=0,
     tags=["button"]}`, flipper_right → `"button_flipper_right"`, launch →
     `"button_launch"`. Releases and autorepeat emit nothing; a press is
     emitted even while that flipper is disabled. These events are pushed
     with all others in step 7.

  2. FLIPPER STATE UPDATE                       (§5.2)
     For each flipper in id order: advance the state machine, compute this
     tick's angular velocity ω (held constant within the tick) and start
     angle θ0. Record (θ0, ω) for moving-capsule CCD.
     Advance other kinematic animations: drop-target raise/drop timers,
     spinner plate angle, kicker/plunger visual state, gate arm.

  3. FORCES + VELOCITY UPDATE                   (§1.3)
     For each FREE ball in index order: apply accelerations 1–5, damp spin,
     clamp speed. For each RAMP ball: apply the 1-D dynamics (§6.10.4).
     CAPTURED balls: decrement hold timers.

  4. CCD RESOLUTION LOOP                        (§3.6)
     Move all FREE balls through the tick on a shared timeline, resolving
     earliest-TOI contacts (max kMaxToiIter per ball), with fallback
     push-out (§3.8). RAMP balls advance s ← s + s_dot·dt and resolve 1-D
     end/ball collisions (§6.10.5).

  5. CONSTRAINT PROJECTION
     RAMP balls: recompute exact 2-D pos (and z) from s — position is
     always derived from s, so path drift is impossible. Captive balls:
     project velocity onto slot axis, integrate and clamp s_c to the slot,
     and emit `captive_full_travel` on a qualifying far-end arrival
     (§6.13). Ramp bind/unbind checks for FREE balls that crossed an entry
     seam this tick, each seam tested on its derived layer
     (§6.10.2, §6.10.6).

  6. TRIGGERS AND REGIONS
     In element id order (ascending, lexicographic by id string), for each
     ball in index order: evaluate rollovers, outhole, kicker capture,
     ball_lock capture, spinner pass, plunger contact zone, magnet region
     entry (for spin damping flag). Fire element state changes (drop
     target down, bank complete, kicker eject, plunger launch).

  7. EVENT DISPATCH + SNAPSHOT               (tick phases 2, 3, 4)
     7a. PHASE 2 — dispatch sim events to Lua.
         Push all SimEvents generated this tick (in generation order —
         deterministic because steps 4 and 6 are ordered) into the event
         ring buffer, then run the Lua handlers for them in that same
         emission order, handlers per event in registration order
         (10-scripting.md §2.2).
     7b. PHASE 3 — step the GameFsm (11-game-framework.md §1).
         The FSM consumes this tick's sim events and runs its
         transitions. Every framework-originated event — `game_start`,
         `ball_start`, `ball_end`, `player_up`, `game_end`,
         `tilt_warning`, `tilt`, `multiball_start`, `multiball_end`,
         `ball_save_expired`, `timer_tick` — is dispatched to Lua
         **synchronously as the FSM emits it, in emission order, within
         this same tick**; never queued for tick n+1. That synchronous
         dispatch is what makes 11-game-framework.md §4.5's "scripts may
         call `tb.add_bonus` inside the `ball_end` handler"
         implementable.
     7c. PHASE 4 — fire this tick's expired script timers (deadline == n,
         ascending timer_id), run one incremental `lua_gc` STEP, and
         publish the triple-buffered SimSnapshot (canon §5.4). Update
         per-ball last_safe_pos if the ball ended the tick
         penetration-free.

     Framework event **emission order** (phase 3) is part of the
     deterministic replay record, exactly like ball state (§2.2 rule 7).
     Physical actions requested from any phase-2/3/4 handler are latched,
     not applied — they take effect at the start of tick n+1, step 1.
```

### 2.2 Determinism rules (binding)

Canon §5.3: same binary + same seed + same input stream ⇒ identical
simulation. Cross-platform bit-exactness is a non-goal, but same-binary
determinism is a hard requirement, protected by these rules:

1. **Ordered iteration only.** Elements are stored in a `std::vector`
   sorted once at load by id (ascending `std::string` compare); balls by
   index. No `std::unordered_map`/`unordered_set` on any per-tick path. No
   iteration keyed on pointer values.
2. **No fast math.** Compile `tb_sim` with: MSVC `/fp:precise` (set
   explicitly; never `/fp:fast`); Clang/GCC: never `-ffast-math`,
   `-funsafe-math-optimizations`, or `-Ofast`, and set
   `-ffp-contract=off` on the `tb_sim` target to stop FMA contraction from
   varying with inlining decisions.
3. **No wall clock.** The sim never calls any time API. All durations are
   tick counts (`ms` parameters are converted to ticks at load:
   `ticks = round(ms / 1.0)`).
4. **Exactly two sim-owned RNG streams** (05-engine-core.md §10.1), both
   PCG32 instances seeded from `game_seed` at game start: `rng_sim` for
   physics-side randomness (pop-bumper jitter §6.3) and `rng_script`
   backing Lua `tb.rng` — separate streams so script draws never perturb
   physics sequences. Because draw sites execute in deterministic order,
   both streams are reproducible. Never create any other RNG.
5. **Input is latched once** per tick as an atomic snapshot (step 1). No
   other thread's data is read anywhere else in the tick.
6. **Floats are never left uninitialized**; all state is value-initialized.
7. The replay format (16-testing-ci.md) is `(seed, per-tick latched input
   words)`; replaying must reproduce every ball position bit-for-bit
   **and** the same per-tick event sequence — sim event generation order
   (phase 2) and the framework's event emission order (phase 3, §2.1
   step 7b) are both part of the deterministic record.

## 3. Continuous collision detection

A ball at 12 m/s moves 12 mm per tick — nearly its own diameter — so every
ball-vs-collider test is a **swept** (continuous) test returning a time of
impact (TOI) `t ∈ [0, dt]`. Discrete overlap tests are used only by the
push-out fallback (§3.8).

### 3.1 Conventions and collider set

- `perp(v) = (−v.y, v.x)` (90° CCW). `cross(a,b) = a.x·b.y − a.y·b.x`.
- Contact normal `n̂` always points **from the collider surface toward the
  ball center**.
- Static colliders are baked at table load (09-table-format.md geometry →
  primitives): every wall path becomes segments + arcs, plus **point
  colliders at every node** (corner caps) and at every arc endpoint.
  Segments and arcs are infinitely thin and collide on both sides.
- Each collider carries: `element_id`, `sub_index` (position within the
  element's primitive list), `material`, `layer`. A ball only tests
  colliders with `collider.layer == ball.layer`.
- Arc baking: a path node `{"arc": {"to": P1, "radius": R, "dir": d}}`
  following point `P0` becomes an arc with center
  `C = M ± h·perp(normalize(P1−P0))`, where `M = (P0+P1)/2`,
  `h = sqrt(R² − |P1−P0|²/4)`, `+` for `"ccw"`, `−` for `"cw"`
  (09-table-format.md validates `|P1−P0| ≤ 2R`). Internally every arc is
  stored CCW as `(C, R, θ_start, Δθ_ccw ∈ (0, 2π])`; a `"cw"` arc is
  stored with its endpoints swapped.

### 3.2 Swept circle vs segment

Segment `A→B`, `d = B − A`, `d̂ = d/|d|`, unit normal `n̂ = perp(d̂)`. Ball
center `P(t) = P0 + v·t`, radius `r`, window `t ∈ [t0, t1]`.

```
sn0 = dot(P0 − A, n̂)          // signed distance to the line
vn  = dot(v, n̂)
σ   = (sn0 >= 0) ? +1 : −1     // which face the ball is on
if |sn0| < r:                   // already inside the slab
    if vn·σ < 0 and tangential_window(P0): return TOI = t0  (immediate)
    else: no hit from the line (endpoints may still hit)
if vn·σ >= 0: no line hit       // moving away or parallel
t_hit = (σ·r − sn0) / vn
if t_hit < t0 − kToiEps or t_hit > t1: no line hit
Q = P0 + v·t_hit
u = dot(Q − A, d̂)
if 0 <= u <= |d|: HIT at t_hit, normal = σ·n̂
else: no line hit               // fall through to endpoint point tests
```

Endpoint caps `A` and `B` are tested with §3.3 (they are baked as point
colliders, so this happens naturally; do not double-count — a point collider
at a node shared by two segments exists once).

```
      n̂ = σ·perp(d̂)                 ball P0
        ^                              o →v
        |                             /
  A ----+---------------------- B    / t_hit
        <——— tangential window ———> ●  contact, |sn| = r
  (u<0: test point A)   (u>|d|: test point B)
```

### 3.3 Swept circle vs point (posts, corners)

Point `C`, effective radius `ρ` (= `r` for a bare corner; `r + post.radius`
for a post — posts are circle colliders, equivalent to a point with enlarged
radius).

```
w = P0 − C
a = dot(v, v);  b = 2·dot(w, v);  c = dot(w, w) − ρ²
if c < 0:                        // already overlapping
    if b < 0: return TOI = t0 with n̂ = normalize(w)  (immediate)
    else: no hit
if b >= 0: no hit                // moving away
disc = b² − 4·a·c
if disc < 0: no hit
t_hit = (−b − sqrt(disc)) / (2·a)
if t0 − kToiEps <= t_hit <= t1: HIT, n̂ = normalize(P0 + v·t_hit − C)
```

### 3.4 Swept circle vs arc

Arc `(C, R, θ_start, Δθ)` (CCW). Two contact surfaces:

- **Outer** (ball outside the circle, convex side): effective radius
  `ρ_out = R + r`.
- **Inner** (ball inside the circle, concave side — orbit guides): requires
  `R > r`; effective radius `ρ_in = R − r`.

Solve `|P0 + v·t − C| = ρ` with the §3.3 quadratic (`w = P0 − C`,
`c = dot(w,w) − ρ²`):

```
outer: valid only if dot(w,w) > ρ_out²; take root (−b − sqrt(disc))/(2a)
       (approaching from outside needs b < 0);  n̂ = normalize(Q − C)
inner: valid only if dot(w,w) < ρ_in²;  c < 0 so disc > 0; take the
       positive root (−b + sqrt(disc))/(2a);   n̂ = normalize(C − Q)
```

Angular window check at the hit point `Q`:
`φ = atan2((Q−C).y, (Q−C).x)`; hit is valid iff
`wrap_ccw(φ − θ_start) ∈ [0, Δθ]` where `wrap_ccw` maps to `[0, 2π)`.
Outside the window: no arc hit (the baked endpoint point colliders cover the
arc tips).

### 3.5 Swept circle vs moving tapered capsule (the flipper)

The flipper collider is a **tapered capsule** ("uneven capsule"): base circle
radius `r_b` centered at the pivot, tip circle radius `r_t` centered at
distance `L` along the flipper axis, hulled by the two external tangents.
The capsule rotates about the fixed pivot with angular velocity `ω`
(piecewise-constant within a tick, set in step 2): `θ(t) = θ0 + ω·t`.

**Local frame:** origin at pivot, +x along the flipper axis.
`p_local(t) = R(−θ(t)) · (P_ball(t) − pivot)`, `P_ball(t) = P0 + v·t`.

**Signed distance** (adapted uneven-capsule SDF; `q = (|p.y|, p.x)` maps
cross-axis then along-axis):

```
sd_flipper(p):                       // p in local frame
  k  = (r_b − r_t) / L               // taper sine, 0.0526 at defaults
  a  = sqrt(1 − k²)
  qc = |p.y| ;  ql = p.x             // cross, along
  m  = −k·qc + a·ql                  // projection selecting the region
  if m < 0:        return length(p) − r_b               // base cap
  if m > a·L:      return length(p − (L, 0)) − r_t      // tip cap
  else:            return a·qc + k·ql − r_b             // side face
```

Local surface normal per branch: base `normalize(p)`; tip
`normalize(p − (L,0))`; side:

```
side branch:  n_local = ( k, a·sign(p.y) )   // x-component k (toward tip
                                             // taper), y-component ±a
check: untapered (k = 0) gives (0, ±1) — perpendicular to the axis. ✓
world normal: n̂ = R(θ) · n_local  (then normalize defensively)
```

Exact TOI against a rotating surface is transcendental, so use
**conservative advancement**:

```
toi_flipper(ball, flipper, t0, t1):
  t = t0
  bound = |v| + |ω| · (L + max(r_b, r_t))    // max closing speed
  repeat up to 24 times:
    p = local(P0 + v·t, θ(t))
    d = sd_flipper(p) − r                     // separation
    if d < kSkin: return TOI = t              // contact
    if bound <= 0: return no hit
    t += max(d / bound, 1e-6)                 // never step less than 1 µs
    if t > t1: return no hit
  return TOI = t                              // converged tight: treat as hit
```

**Relative-velocity fast path:** when `|ω| < 1 rad/s` (rest, hold, or
end-of-drop), treat the capsule as static at `θ0` and run a swept test
against the two cap circles (§3.3 with `ρ = r + r_b` / `r + r_t`) and the
two tangent side segments (§3.2 on the tangent lines offset by the taper).
This is exact and cheaper; the conservative advancement path is mandatory
whenever `|ω| ≥ 1 rad/s`.

**Contact data:** ball center at TOI `P_c`; `n̂` from the branch normal;
contact point `X = P_c − r·n̂`; **surface velocity**
`V_s = ω · perp(X − pivot)` (m/s — this is what makes flippers transfer
energy, §5.3).

```
            side face (tangent line)
   base  ______________________
  r_b=11mm                      `––– tip r_t = 7 mm
   ( pivot●———————— axis, L = 76 mm ————●tip )
         θ from +x ______________________
                    taper angle φ = asin((r_b−r_t)/L) ≈ 3.0°
```

### 3.6 The CCD resolution loop (shared timeline)

All FREE balls advance together through the tick. One loop resolves the
globally earliest contact each iteration:

```
ccd_phase():
  t_cur = 0
  for each FREE ball: resolved[ball] = 0
  loop:
    best = none
    for each FREE ball i in index order (skip frozen):
      candidates = broadphase_query(ball i, t_cur → dt)      // §3.7, sorted
      for each candidate c in (element_id, sub_index) order:
        toi = swept_test(ball i, c, t_cur, dt)               // §3.2–3.5
        best = min(best, (toi, kind=STATIC, i, c))           // strict order
    for each pair (i, j), i < j, both FREE, same layer:
      toi = swept_pair_test(i, j, t_cur, dt)                 // §8
      best = min(best, (toi, kind=PAIR, i, j))
    if best == none:
      advance every FREE ball: pos += vel · (dt − t_cur);  break
    // advance ALL free balls to the earliest TOI
    t_adv = best.toi − kToiEps
    for each FREE ball: pos += vel · (t_adv − t_cur)
    t_cur = t_adv
    pull the hitting ball back along v so separation ≥ kSkin (see below)
    resolve_contact(best)                                    // §4 impulses
    clamp speed; resolved[ball]++ (both balls for PAIR)
    if resolved[ball] > kMaxToiIter:
      freeze ball: it keeps its velocity but its position stops advancing
      for the remainder of this tick (count in SimTickStats.frozen)
```

Tie-breaking (`min` above): compare `(toi, kind, element_id or pair ids,
ball index)` lexicographically — STATIC before PAIR at equal TOI, then lower
element id, then lower ball index. This makes simultaneous contacts
deterministic.

Pull-back: `t_back = kSkin / |v|` (capped so `t_cur − t_back ≥ 0`); position
the ball at `P(t_cur − t_back)`. This keeps the ball `kSkin` off the surface
so the same contact cannot re-fire at TOI 0 forever.

RAMP-mode balls skip this loop entirely (they are not in the broadphase).

### 3.7 Broadphase: uniform grid

- Cell size `kGridCell = 0.032 m` (≈ 2.4 ball radii). Grid dimensions =
  `ceil(width/0.032) × ceil(height/0.032)` (default table: 17 × 33). One
  grid per layer that has colliders.
- **Baked at load:** each static collider is inserted into every cell its
  AABB overlaps (AABB inflated by `kBallRadius + kSkin` so queries never
  miss a grazing contact). Per-cell lists are sorted by
  `(element_id, sub_index)`.
- **Query:** the ball's swept AABB = AABB of `P(t_cur)` and `P(dt)` inflated
  by `r + kSkin`. Collect colliders from every overlapped cell; dedupe with
  a per-query stamp array (collider index → last query id); output sorted
  by `(element_id, sub_index)` (merge of sorted cell lists preserves order;
  after dedupe, re-sort the ≤ 96-entry candidate buffer to be safe).
- **Dynamic colliders bypass the grid:** flippers, gates (when blocking),
  drop targets (when up), plunger face. There are ≤ ~12 of these per table;
  test each whose swept AABB (for flippers: AABB of the capsule at θ0 and
  θ0+ω·dt, inflated by ball travel) overlaps the ball's swept AABB. Iterate
  them in element-id order.
- Ball-ball pairs need no broadphase (≤ 15 pairs at 6 balls).

### 3.8 Fallback push-out (depenetration)

Runs at the start of step 4 for each FREE ball, and after any teleport-like
state change (eject, layer change):

```
for iter in 0..3:
  find deepest static overlap (discrete distance query on broadphase
  candidates at the current position, id order; ties → lower id)
  if none or depth <= 0: done
  pos += n̂ · (depth + kSkin)
if still penetrating after 4 iterations:
  pos = last_safe_pos        // stored per ball, §2.1 step 7
```

Push-out **never modifies velocity or spin** — it is a position correction
only. Injecting velocity here is the classic energy-leak bug (see Common
pitfalls).

## 4. Contact resolution

### 4.1 Impulse equations (ball vs surface)

Inputs: normal `n̂` (surface → ball center), tangent `t̂ = perp(n̂)`, surface
velocity `V_s` (zero for static colliders; §3.5 for flippers; §8 for balls),
material `(e, μ_s, μ_k, κ)` (§4.3).

```
c   = −r·n̂                        // ball center → contact point
v_p = v + omega_z · perp(c)        // ball surface-point velocity
    = v − r·omega_z · perp(n̂)
u   = v_p − V_s                    // relative contact velocity
u_n = dot(u, n̂) ;  u_t = dot(u, t̂)
if u_n >= 0: return                // separating — NEVER apply impulses

e_eff = restitution_curve(e, −u_n)             // §4.2
j_n   = −(1 + e_eff) · u_n · m                 // normal impulse (≥ 0)

// tangential effective mass for a solid sphere: 1/m_t = 1/m + r²/I = 7/(2m)
j_stick = −(2·m/7) · u_t                       // impulse that stops sliding
if |j_stick| <= μ_s · j_n:  j_t = j_stick      // static: contact sticks
else:                       j_t = −sign(u_t) · μ_k · j_n   // kinetic slide

v       += (j_n·n̂ + j_t·t̂) / m
omega_z += κ · (−r · j_t) / I      // κ = material spin_transfer ∈ [0,1]
clamp speed and spin
```

Derivation note for the spin term: torque impulse `= cross(c, j_t·t̂)`
`= −r·j_t·cross(n̂, t̂) = −r·j_t` (since `cross(n̂, perp(n̂)) = 1`). The
`κ` factor scales how much tangential impulse becomes stored spin; `κ < 1`
discards (dissipates) part of the spin change — this is lossy, never
energy-adding, and models slick materials.

Emission: every resolved static contact with `−u_n ≥ 0.25 m/s` emits a
`SimEvent{collision, element_id, ball, speed=−u_n, n̂}` for audio/particles
(not a Lua gameplay event; gameplay events are per-element, §6).

### 4.2 Velocity-dependent restitution

Real pinball surfaces (and VPX's `elasticityFalloff`) bounce proportionally
less at high impact speed. For approach speed `s = −u_n` (m/s):

```
restitution_curve(e, s):
  if s < kRestSpeed (0.15): return 0            // resting contact, no bounce
  soft_scale = min(1, (s − kRestSpeed) / (kSoft − kRestSpeed))   // ADR-021
  return e · soft_scale / (1 + kFalloff · max(0, s − kSoft))
kSoft    = 0.5 m/s     // full elasticity above the low-speed ramp
kFalloff = 0.12 s/m    // e halves around ~9 m/s over kSoft
```

The low-speed ramp (ADR-021) is a viscoelastic cliff: real rubber is
velocity-weakening at small impact speeds, and a flat e down to the cutoff
sustains a micro-bounce limit cycle — caught balls rattle at the sweep
threshold instead of settling. Impacts at or above kSoft are unchanged by
the ramp.

Both constants are global defaults, per-table overridable as
`physics.restitution_falloff` (= `kFalloff`, s/m, default 0.12) and
`physics.restitution_soft` (= `kSoft`, m/s, default 0.5) — the §1.3
override table lists them and **09-table-format.md §2 is the schema owner**
(range, default, validation). Acceptance: a ball hitting rubber (`e = 0.75`)
at 1.0 m/s rebounds at `0.75/(1+0.12·0.5) = 0.708` of approach speed
(±0.01); at 8.0 m/s it rebounds at `0.75/1.9 = 0.395` (±0.01).

### 4.3 Materials table

| material | e | μ_s | μ_k | spin_transfer κ | used for |
|---|---|---|---|---|---|
| `wood` | 0.30 | 0.25 | 0.15 | 0.60 | painted wood walls (wall default) |
| `steel` | 0.45 | 0.15 | 0.10 | 0.50 | metal guides, rails, plunger tip |
| `rubber` | 0.75 | 0.60 | 0.45 | 0.90 | post/wall rubbers, sling face |
| `plastic` | 0.35 | 0.20 | 0.12 | 0.40 | ramps, targets, toy colliders |
| `flipper_rubber` | 0.85 | 0.60 | 0.45 | 0.90 | flippers only (implicit) |

Ball-ball contact constants (§8): `e = 0.93`, `μ_s = μ_k = 0.05`,
`κ = 0.20`. Tables may override any material row under `"materials"` in
`table.json` (09-table-format.md); unknown material names are a validation
error.

### 4.4 The energy rule (testable property)

Define ball kinetic energy `KE = ½·m·|v|² + ½·I·omega_z²`.

**Passive contacts never add energy.** For any contact whose surface
velocity `V_s = 0` and which applies no scripted/powered kick (walls, posts,
arcs, resting flippers, targets, dead ball-ball hits with the pair's total
KE), the total KE after resolution must satisfy
`KE_after ≤ KE_before + 1e-9 J`. This is enforced by a property test
(16-testing-ci.md): 10⁶ random passive impacts across all materials and
geometries; any violation is a solver bug, not a tuning issue. Active
elements (powered flipper strokes, slingshots, pops, kickers, plunger,
magnets, nudge) are exempt — they model solenoids and springs.

## 5. Flippers

Flippers are the highest-value feel surface in the project (ARCHITECTURE.md
ADR-003). This section is binding down to the curve shapes.

### 5.1 Geometry and conventions

Registry parameters (09-table-format.md): `pos` (pivot), `length`
`L = 0.076`, `radius_base` `r_b = 0.011`, `radius_tip` `r_t = 0.007`,
`rest_angle_deg`, `swing_deg = 52` (positive = up-swing), `side`
(`"left"|"right"`), `input`, `strength = 1.0`.

Angle convention: the flipper axis direction is `(cos θ, sin θ)`, θ in
radians CCW from +x, **in world table space**. `rest_angle_deg` is the
authored θ at rest. The stroke progress `p ∈ [0, 1]` maps to:

```
θ(p) = θ_rest + side_sign · swing · p
side_sign = +1 for side "left" (up-swing is CCW)
          = −1 for side "right" (up-swing is CW)
```

Typical authoring: left flipper `rest_angle_deg = −31` (tip points
down-right toward the drain), fully raised at `−31 + 52 = +21`. Mirrored
right flipper: `rest_angle_deg = 211`, raised at `211 − 52 = 159`.

The collider is the tapered capsule of §3.5, material `flipper_rubber`.

### 5.2 State machine and stroke profiles

States: `REST → RISING → HOLD → DROPPING → REST`, driven by the flipper's
logical button (`input` mapping; a disabled flipper — `tb.set_flipper_enabled
(id, false)` or tilt — behaves as button-always-released).

```
REST:     p = 0, ω = 0. Button pressed → RISING (t_press = now).
RISING:   ω(t) = side_sign · ω_max · min(1, t_since_press / τ_rise)
          ω_max = 42 rad/s · strength ;  τ_rise = 0.011 s
          (linear ramp = constant angular acceleration
           α_rise = ω_max/τ_rise ≈ 3818 rad/s², then constant ω_max).
          p integrates: p += |ω|·dt / swing_rad.
          p reaches 1 → clamp p = 1, ω = 0, state = HOLD, record t_eos.
          Button released mid-rise → DROPPING (from current p).
HOLD:     p = 1, ω = 0. The flipper is kinematically rigid: ball impulses
          never move it (hold torque is modeled as infinite; do NOT add
          compliance or spring-back in v1).
          Button released → DROPPING (t_release = now).
DROPPING: constant angular acceleration α_drop = 2400 rad/s² toward rest,
          capped at |ω| ≤ ω_drop_max = 24 rad/s.
          p reaches 0 → clamp p = 0, ω = 0, state = REST (no end bounce).
          Button pressed mid-drop → RISING, with the ramp resuming from
          the current |ω| if it already exceeds the ramp value:
          effective t_since_press = |ω_current|/ω_max · τ_rise.
```

Within a tick, ω is evaluated once (step 2) and held constant; θ advances
`θ += ω·dt` after CCD used it. Full-stroke timing at defaults: rise covers
52° in ≈ 27 ms (ramp phase 0.23 rad + 42 rad/s cruise); drop returns in
≈ 43 ms. These durations are emergent — do not hardcode them.

`swing_rad = swing_deg · π/180`. `strength` scales `ω_max` only (not
α_drop, not hold).

### 5.3 Ball-flipper contact: surface-velocity impulse transfer

Ball-flipper contacts are found by §3.5 and resolved by §4.1 with the
flipper as an **infinite-mass moving surface**: `V_s = ω · perp(X − pivot)`
at contact point `X`. No special-case "flip force" exists — all energy
transfer flows through `V_s` in the standard impulse equations. This is what
produces the real-machine behaviors:

- Contact radius scales power: `|V_s| = |ω| · ρ`, `ρ = |X − pivot|`. A tip
  hit (`ρ ≈ L + r_t = 0.083 m`) at full `ω_max = 42 rad/s` has surface
  speed 3.49 m/s; a base hit (`ρ ≈ 0.03 m`) only 1.26 m/s. Exit speed for
  a head-on hit follows from §4.1:
  `v'_n = V_s,n + e_eff · (V_s,n − v_n)` — approximately
  `(1 + e_eff)·|V_s|` plus reflected approach speed. Tip shots at defaults
  reach 5–8 m/s; base (backhand) shots 2.5–4 m/s.
- A resting or held flipper (`ω = 0`) is just a rubber wall — balls bounce
  per materials, enabling dead bounces and cradles.
- Friction + `κ = 0.9` puts strong spin on flipped balls, which alters the
  next wall bounce (english).

### 5.4 Live catch damping

A live catch (raising the flipper *just* before the ball lands so it dies on
the rubber) needs help beyond raw restitution to feel learnable:

```
if contact occurs while state == HOLD
   and (now − t_eos) <= kLiveCatchWindow (0.050 s):
     e_eff ← e_eff · kLiveCatchFactor (0.15)
     μ_s, μ_k ← min(1.0, 2·μ)          // enforce tangential grip
```

The window starts when the stroke reaches end-of-stroke (`t_eos`), so a
flipper raised long ago (a waiting cradle) gives a normal bounce, while a
just-raised one absorbs the ball. Both constants are per-table overridable:
`physics.live_catch_window_ms` (the 0.050 s window written in ms, default
50, converted to ticks at load like every other `ms` parameter, §2.2 rule 3)
and `physics.live_catch_factor` (the 0.15 multiplier on `e_eff`, default
0.15) — listed in the §1.3 override table, with
**09-table-format.md §2 the schema owner** (range, default, validation).

### 5.5 Flipper parameter table

| Parameter | Default | Range | Effect |
|---|---|---|---|
| `length` | 0.076 m | 0.05–0.09 | reach; tip speed at fixed ω |
| `radius_base` | 0.011 m | 0.008–0.014 | base hit geometry, cradle pocket |
| `radius_tip` | 0.007 m | 0.005–0.010 | tip hit geometry |
| `swing_deg` | 52 | 40–70 | stroke arc |
| `strength` | 1.0 | 0.5–1.5 | scales ω_max (upper flippers ~0.85) |
| `ω_max` (derived) | 42 rad/s | — | peak angular speed |
| `τ_rise` (global) | 0.011 s | 0.008–0.016 | ramp-up time |
| `α_drop` (global) | 2400 rad/s² | 1500–3500 | release acceleration |
| `ω_drop_max` (global) | 24 rad/s | 16–32 | release speed cap |
| live catch window | 0.050 s | 0.03–0.08 | §5.4 |
| live catch factor | 0.15 | 0.05–0.30 | §5.4 |

### 5.6 The feel-test rig

This section is **normative** for the harness (16-testing-ci.md §2.5 and
04-milestones.md M4 conform to it). All FT scenarios run headless on a
fixed rig built in test code — **no `table.json`, no `.tbreplay` tape** —
seed `0x54425354`:

```
play area 0.52 × 1.04 m, slope 6.5°, default materials/constants
flipper_l : pivot (0.170, 0.120)  rest −31°  swing 52  side left
flipper_r : pivot (0.350, 0.120)  rest 211°  swing 52  side right
wall in_l : [(0.148, 0.300), (0.166, 0.140)]  wood   // left inlane guide
wall in_r : [(0.372, 0.300), (0.354, 0.140)]  wood   // right inlane guide
post_l    : (0.148, 0.300) r 0.008 rubber
post_r    : (0.372, 0.300) r 0.008 rubber
wall border: play-area rectangle, closed, wood
outhole   : a (0.20, 0.015)  b (0.32, 0.015)

           in_l \            / in_r
                 \          /
                  ●        ●            ● = posts
                   \      /
   pivot_l ●——tip  gap  tip——● pivot_r
        (0.170,0.120)    (0.350,0.120)
              tips ~50 mm apart (drain gap)
```

M8 additions (present only for FT-09/FT-10; absent from the M4 rig so the
M4 scenarios are unaffected):

```
magnet_m : pos (0.260, 0.720)  radius 0.09  strength 1.2   // §6.12 defaults
ramp_r   : straight path (0.300, 0.300) → (0.300, 0.900)   // S = 0.6 m
           height_profile [{s:0, z:0}, {s:1, z:0.08}]
           entry seam at s = 0 on layer 0; far end drop_exit: true
```

Input scripts are **state-triggered**: "press when ball.y ≤ Y" means the
harness checks the predicate after each tick and injects the input for the
next tick. This is deterministic and survives constant tuning.

Helper predicate `cradled(flipper)`: over the last 500 ticks, `|v| < 0.05
m/s` and distance(ball surface, flipper surface) `< 0.002 m`.
`CRADLE_SETUP` := spawn ball at (0.185, 0.165), v = 0; press left at t = 0
and hold; require `cradled(flipper_l)` within 1.5 s (this is itself FT-03's
core).

### 5.7 Feel test scenarios FT-01…FT-10

Every scenario is an automated gtest (16-testing-ci.md). Each scenario
carries a milestone tag (04-milestones.md): **FT-01…FT-08 are tagged M4**
(flipper feel); **FT-09 and FT-10 are tagged M8** (magnet and ramp feel).
Bands are the acceptance contract; if a band fails, apply §5.8 — never
widen a band without a JOURNAL.md entry.

**FT-01 Dead bounce (M4).** No input. Ball at (0.205, 0.220), v = (0, −1.2).
It falls onto the resting left flipper.
Expect: rebound speed (measured 10 ms after first flipper contact) in
**[0.75, 1.15] m/s**; `v.x > 0.4 m/s` (bounces toward the right flipper);
ball reaches `x ≥ 0.28 m` within 400 ms of contact; ball never re-touches
the left flipper within those 400 ms.

**FT-02 Live catch (M4).** Ball at (0.205, 0.50), v = (0, −2.5). Press left when
`ball.y ≤ 0.26` and hold.
Expect: flipper reaches HOLD before contact; within 150 ms after first
contact, `|v| < 0.40 m/s`; ball remains in contact (distance < 0.003 m)
with `|v| < 0.10 m/s` from 0.5 s to 2.0 s after contact (a caught ball).

**FT-03 Cradle hold (M4).** `CRADLE_SETUP`, then keep holding 3 s.
Expect: `cradled(flipper_l)` true from t = 1.5 s onward; over t ∈ [2, 3] s
the ball center moves < **1 mm** total (no jitter); penetration into the
flipper ≤ 0.3 mm at all ticks; ball does not roll off the tip
(`x < 0.235` throughout).

**FT-04 Backhand (M4).** `CRADLE_SETUP`; at t = 2.0 s release left; re-press
left at t = 2.050 s and hold.
Expect (ADR-022): from the cradle crook the re-stroke scoops the ball up
off the blade base — the inlane-wall pocket forces a late, EOS-coincident
ejection, so this is a soft scoop, not a power shot. The flipper reaches
HOLD within 50 ms of the re-press; launch speed (max |v| over the first
100 ms of the re-press) ∈ **[0.8, 1.5] m/s**; the ball rises above
`y = 0.22` within 300 ms of the re-press; the ball never drains through
the center gap.

**FT-05 Post pass (flipper-to-flipper transfer, low arc) (M4).**
`CRADLE_SETUP`;
at t = 2.0 s release left; re-press left at t = 2.070 s, release it at
t = 2.150 s; press right at t = 2.150 s and hold.
Expect: ball crosses to `x > 0.30` with apex `y_max ∈ [0.18, 0.48]`
(a pass, not a shot); ball settles on the raised right flipper
(`cradled(flipper_r)`) within 2.5 s of the re-press; ball never drains.

**FT-06 Tap pass (gentle lob transfer) (M4).** `CRADLE_SETUP`; at t = 2.0 s
release left; press left at t = 2.030 s for 50 ms, then release; press
right at t = 2.120 s and hold.
Expect (ADR-022): the tap is absorbed at the crook — a dead-soft touch.
Ball leaves the left flipper at ≤ **2.0 m/s**; apex `y_max ≤ 0.36`; the
ball transfers rightward and comes to rest in the right zone
(`x > 0.25`, `y < 0.10`, `|v| < 0.05 m/s`) within 2.5 s; never drains.

**FT-07 Tip shot power (M4).** Ball at (0.240, 0.55), v = (0, −2.0). Press left
when `ball.y ≤ 0.175` and hold 100 ms.
Expect: contact occurs at `ρ ≥ 0.055 m` from the pivot; exit speed
(10 ms after contact) in **[4.5, 8.5] m/s**; ball crosses `y = 0.95`
within 0.5 s of contact; exit speed ≥ 1.5× the FT-04 launch speed.

**FT-08 Cradle escape via slap (M4).** `CRADLE_SETUP`; at t = 2.0 s release
left (ball rolls down the flipper toward the tip, gaining speed); re-press
left at t = 2.140 s and hold.
Expect (ADR-022): the slap ejects the ball upward off the blade — the
wall pocket forces a late ejection, so this verifies the escape itself:
exit speed (max |v| within 200 ms of the re-press) ∈ **[0.3, 3.0] m/s**;
the ball climbs at least 50 mm above its pre-slap height; it crosses
`y ≥ 0.20` within 1.0 s of the re-press; never drains through the center
gap.

**FT-09 Magnet catch and throw (M8).** Rig plus M8 additions; no flipper
input. Ball at (0.260, 0.860), v = (0, −0.40) — it rolls down into the
field (edge at y = 0.810, entering at ≈ 0.52 m/s, well below the field's
escape speed). The harness energizes `magnet_m` at t = 0 and de-energizes
it at t = 4.0 s (script equivalent: `tb.magnet_on`/`tb.magnet_off`; a
`tb.magnet_pulse` grab-and-throw uses the same field with the §6.12 pulse
envelope).
Expect: **catch** — once inside the field the ball never leaves it while
energized (`|ball.pos − magnet_m.pos| ≤ 0.09 m`); by 3.0 s after entry,
`|v| ≤ 0.35 m/s` and `|ball.pos − magnet_m.pos| ≤ 0.03 m`, and both
bounds hold until release. **No field energy injection** — `|v|` never
exceeds **3.5 m/s** at any tick (the §6.12 well converts at most
≈ 2.4 m/s at the core). **Throw (release)** — across the release tick,
`|Δv| ≤ 0.08 m/s` (release adds no impulse; the bound is one tick of the
clamped field acceleration, 60 m/s² · dt, plus slack); after release the
ball leaves the field down-table and crosses `y = 0.45` with `v.y < 0`
within 2.0 s of release.

**FT-10 Ramp make and rollback (M8).** Rig plus M8 additions; no input.
This scenario owns the Done-when ramp integration numbers.
(a) **Make:** ball at (0.300, 0.270), v = (0, +4.0) — crosses the entry
seam at ≈ 4.0 m/s, inside the §6.10.2 alignment and speed gates.
Expect: the ball binds exactly once; forward exit (`s > S`) emits
`switch_hit` then `ramp_made` (§6.10.6) with exit `s_dot` in
**[3.0, 3.7] m/s**; transit time (bind → exit) ≤ 250 ms; smoothness —
while bound, per-tick `|Δs_dot| ≤ 0.01 m/s` except the bind tick itself
(the 5% seam trim); the ball-center position is continuous at the bind
and exit ticks (jump ≤ 0.006 m).
(b) **Rollback:** same setup, v = (0, +1.2) (≈ 1.17 m/s at the seam).
Expect: the ball binds once, climbs, stalls (`s_dot` crosses 0 with
`s < S`), rolls back and unbinds out the entry (`s < 0`, or fall-back-off
per §6.10.6) within 1.5 s of binding, exiting down-table (`v.y < 0`); no
`ramp_made` is emitted; the ball never binds twice in one approach.

### 5.8 Tuning procedure

Run the suite in this order — stability first, power second, finesse last:
**FT-03 → FT-01 → FT-02 → FT-07 → FT-04 → FT-08 → FT-05 → FT-06.** Fix the
first failure, re-run the whole suite, repeat. Change **one** parameter at a
time, in steps of ≤ 10% of its default; after 3 changes without progress,
revert all and reconsider the solver (a feel failure is often a §3/§4 bug).

| Failure | Symptom | First knob | Second knob |
|---|---|---|---|
| FT-03 | jitter / sinking | raise `kRestSpeed` (0.03→0.05→…; ADR-021 landed at 0.15) | check kSkin pull-back (§3.6) |
| FT-03 | ball creeps off tip | raise flipper μ_s 0.60→0.70 | lower rest angle 1–2° (rig) |
| FT-01 | too bouncy | raise `kFalloff` 0.12→0.14 | lower flipper e 0.85→0.80 |
| FT-01 | too dead | lower `kFalloff` | raise flipper e |
| FT-02 | ball bounces out | lower live catch factor 0.15→0.10 | widen window 50→70 ms |
| FT-02 | catches too easily (FT-01 regressions) | raise factor | narrow window |
| FT-07 | too weak | raise ω_max 42→46 | lower `kFalloff` |
| FT-07 | too strong / uncontrollable | lower ω_max | raise `kFalloff` |
| FT-04 | wrong direction | verify §3.5 side-branch normal (taper sign) | rig rest angle |
| FT-04 | too weak | raise α_rise (τ_rise 11→9 ms) | raise ω_max |
| FT-05/06 | overshoots (full shot instead of pass) | lower ω_drop_max 24→20 | raise α_drop |
| FT-05/06 | ball drains in the gap | lower ω_drop_max | verify drop has no end bounce |
| FT-08 | too weak | raise flipper κ 0.90→0.95 | raise ω_max |

FT-09/FT-10 (M8) follow the same one-knob discipline: FT-09 failures tune
the §6.12 magnet constants (strength, eddy damping) and FT-10 failures the
§6.10.4 ramp damping terms — never the flipper parameters, which are locked
by the M4 scenarios by then.

Never tune by editing the scenario scripts or bands; they are the contract.

## 6. Element physics

For every element: collider(s), trigger condition, response formula, state,
and event emissions (canonical names, canon §5.7; payload schemas in
10-scripting.md). All thresholds are per-element JSON overridable; defaults
here match the shared registry in 09-table-format.md exactly. All cooldowns
and delays are tick counts converted from `ms` at load.

**Event pairing (binding; 10-scripting.md §4/§4.1 firing rule):** every
physical actuation of a scriptable element pushes `switch_hit{id, ball_id,
speed, tags}` **first**, then its specialized event (if any) immediately
after, in the same tick — this covers slingshot/pop/standup fires, rollover
passes, spinner revolutions, drop-target hits, kicker captures, ball-lock
captures, captive-ball strikes, and ramp exits. `speed` is the ball's speed
in m/s at contact; `tags` is the element's `"tags"` array from `table.json`
(may be empty, never nil). The per-element emission lists below spell the
pair out.

**The single exception** is `captive_full_travel` (§6.13): the captive
ball reaching the far end of its slot is a delayed consequence of a strike
that already emitted its own `switch_hit`, so that event fires alone, on a
later tick. Every other specialized event in §6 is paired.

**Cabinet buttons are switches too (binding, 10-scripting.md §4.1):** each
button **press** edge (not release, not autorepeat) emits a lone
`switch_hit` — no specialized event follows — with `ball_id = 0`,
`speed = 0`, `tags = ["button"]` and id `button_flipper_left` /
`button_flipper_right` (the flipper buttons, which is how lane change works)
or `button_launch` (the launch/plunger action). The sim generates these in
tick step 1 (§2.1); they fire even while the flipper is disabled, and a
`button_launch` press also charges the physical plunger as normal (§6.16).
Suppression during tilt is the framework's job (11-game-framework.md §5).

### 6.1 `wall` and `post`

Pure static colliders (§3.2–§3.4), material per element (`wood` default,
`rubber` for rubbers — rubbers are a material, not a type, canon §5.6).
`closed: true` connects last node to first. Posts are point colliders with
effective radius `r + post.radius` (default post radius 0.008). No trigger,
no events beyond the generic collision SimEvent (§4.1).

### 6.2 `slingshot`

- Geometry: `face` = segment `[a, b]`; collider = that segment (material
  `rubber`) plus its endpoint caps. Active normal
  `n̂_f = normalize(perp(b − a))` — the **left side of a→b**; authors must
  order `a→b` so `n̂_f` points into the playfield (09-table-format.md
  validates this against the play area centroid).
- Trigger: a resolved contact on the active face with approach speed
  `−u_n ≥ 0.4 m/s`, while cooldown expired.
- Response (after the normal §4 resolution of the same contact):
  `v ← v_t·t̂ + max(dot(v, n̂_f), kick_speed)·n̂_f` with
  `kick_speed = 3.5 m/s`. (Sets the outgoing normal speed to at least
  kick_speed; keeps tangential motion.) Active element — may add energy.
- Cooldown 80 ms. Emits `switch_hit{id}` once per trigger, plus a kicked
  visual state in the snapshot for 60 ms (arm animation).

### 6.3 `pop_bumper`

- Collider: circle at `pos`, radius `radius = 0.031 m` (point collider with
  effective radius `r + radius`), material `rubber`.
- Trigger: any resolved contact while cooldown expired (pops fire on the
  lightest touch — no speed threshold).
- Response, after normal resolution: radial kick with deterministic jitter:
  ```
  d̂ = normalize(ball.pos − pos)          // radial, at resolution time
  δ = (rng_sim.next_float() · 2 − 1) · 0.12  // ±0.12 rad ≈ ±6.9°, rng_sim
  v ← v + kick_speed · rot(d̂, δ)          // kick_speed = 4.5 m/s
  ```
  The RNG draw order is deterministic (§2.2 rule 4).
- Cooldown 60 ms per bumper. Emits `switch_hit{id}` and a skirt-flash
  visual state.

### 6.4 `standup_target`

- Collider: segment of length `width = 0.025 m` centered at `pos`,
  perpendicular to `facing_deg` (facing = outward normal, degrees CCW from
  +x), material `plastic`, plus endpoint caps.
- Trigger: resolved contact on the facing side with `−u_n ≥ min_speed`
  (default 0.3 m/s). Contacts on the back side never trigger.
- No state, no kick (targets are passive). Emits `switch_hit{id}`.
  Re-trigger cooldown 100 ms (debounce against chatter).

### 6.5 `drop_target_bank`

- Each entry in `targets` is a standup-style segment (`pos`, `width`,
  `facing_deg`), material `plastic`, with per-target state `UP | DROPPING |
  DOWN | RAISING`.
- Trigger: facing-side contact with `−u_n ≥ 0.3 m/s` while `UP`. The
  triggering contact **is resolved normally** (the ball bounces off the
  target as it starts to drop — real behavior), then the target enters
  `DROPPING`: collider disabled immediately after that resolution; drop
  animation 120 ms (visual only). Emits `switch_hit{id}` (the bank's id)
  then `target_down{bank_id, target_index}` (10-scripting.md §4.1's payload,
  verbatim; `target_index` is 1-based in the bank's declaration order).
- When the last target reaches `DOWN`: emit `bank_complete{bank_id}`.
- Reset (`reset: "script"` via `tb.drop_bank_reset`, or `"auto"` after
  `auto_reset_ms` past bank completion): all targets enter `RAISING`
  (animation 250 ms, collider enabled at completion). If any ball's swept
  circle overlaps any rising target's segment (inflated by kSkin) at reset
  time, the whole reset is deferred and retried every tick until clear —
  never raise a target into a ball.

### 6.6 `spinner`

A spinner is a plate the ball pushes through; it is a **trigger plus a 1-D
plate angular model**, not a solid collider (except when too slow to pass).

- Trigger segment: length 0.025 m centered at `pos`, perpendicular to
  `facing_deg` (facing = the plate's normal axis; ball passes along ±this
  direction).
- On a ball crossing the segment (either direction) with pass speed
  `s_pass = |dot(v, f̂)| ≥ 0.15 m/s`:
  - Plate spin-up: `ω_p ← side · s_pass · 25 rad/s per (m/s)` where
    `side = sign(dot(v, f̂))` (4 m/s shot → 100 rad/s ≈ 16 rev/s).
  - Ball slowdown: `v ← v − 0.12 · sign(dot(v, f̂)) · f̂` (plate inertia),
    magnitude clamped to not reverse `dot(v, f̂)`.
- If `s_pass < 0.15 m/s`: the segment acts as a wall for this contact
  (material `steel`, e forced to 0.3) — the ball bounces back.
- Plate model, every tick: `angle_p += ω_p·dt`;
  `ω_p ← ω_p · friction^dt` (registry `friction = 0.55` = per-second decay
  factor, so `ω_p(t) = ω_p0 · 0.55^t`); stop at `|ω_p| < 0.5 rad/s`.
  Each time `angle_p` accumulates a further 2π since trigger — once per
  **full revolution** (10-scripting.md §4.1) — emit `switch_hit{id}` then
  `spinner_spin{id, rpm}`, where `rpm = |ω_p| · 60 / 2π` at emission time
  (instantaneous revolutions per minute). A 4 m/s rip yields ≈ 26
  revolutions ≈ 26 event pairs over ~5 s. `angle_p` is published in the
  snapshot for rendering.

### 6.7 `gate`

A flap across a lane: segment of length `width` centered at `pos`,
perpendicular to `facing_deg`; `f̂` = the `facing_deg` direction. A gate is
always in exactly one of **three states** (09-table-format.md §4.10),
initialized from `default_state` (default `one_way`):

| State | Ball with `dot(v, f̂) > 0` | Ball with `dot(v, f̂) ≤ 0` |
|---|---|---|
| `one_way` | passes (no collider) | wall |
| `open` | passes (no collider) | passes (no collider) |
| `closed` | wall | wall |

- **Wall** = the segment (plus its endpoint caps) collides as material
  `steel` with restitution forced to 0.3 — gates absorb. Only a gate in a
  blocking state is registered as a dynamic collider (§3.7).
- **Pass** = no collider at all; when the ball center crosses the segment,
  emit `switch_hit{id, ball_id, speed, tags}` once, re-armed when the ball
  center is > 0.03 m from the segment (§6.8-style hysteresis). A ball that
  is blocked emits no switch.
- Control (10-scripting.md §3.4): a gate whose `default_state` is
  `one_way` is **purely mechanical** — it ignores `tb.gate_open` /
  `tb.gate_close` (the call warns and no-ops) and can never leave
  `one_way`. Any other gate is controlled: `tb.gate_open(id)` sets `open`,
  `tb.gate_close(id)` sets `closed`, which blocks **both** directions —
  closing is not a return to one-way flapping.
- The state is published in the snapshot for rendering (the arm swings when
  a ball passes; a `closed` gate renders latched shut).

### 6.8 `rollover`

Wire switch in a lane: capsule region — segment of length `length = 0.05 m`
centered at `pos` along direction `facing_deg`, region radius 0.012 m
(distance from ball **center** to the segment).

- On ball center entering the capsule: emit
  `switch_hit{id, ball_id, speed, tags}` then `rollover{id, ball_id}`
  (10-scripting.md §4.1 payloads, verbatim) once.
- Re-arm only when the ball center leaves an exit capsule of radius
  0.016 m (hysteresis prevents multi-fire from jitter).
- No collider, no effect on the ball.

### 6.9 `kicker` (saucer / scoop / VUK)

- Capture region: circle radius `radius = 0.014 m` (ball **center** within
  it). Capture condition by style: `"saucer"` captures only if
  `|v| < 3.0 m/s` when entering (fast balls fly over — the region is
  ignored); `"scoop"` and `"vuk"` capture at any speed.
- On capture: `mode = CAPTURED`, `pos = kicker.pos`, `v = 0`,
  `omega_z = 0`, `hold_ticks = capture_ms` (default 800 ms,
  09-table-format.md §4.12). Emit `switch_hit{id, ball_id, speed, tags}`
  then `kicker_enter{id, ball_id}`. Letting `hold_ticks` run out is the
  **auto-eject failsafe**: a rules file that does nothing can never
  softlock a ball. A script may eject early with
  `tb.kick(id, speed, angle_deg)`, or take the ball over indefinitely with
  `tb.kick_hold(id)`, which cancels the pending auto-eject
  (`hold_ticks = 0` = script-held, §1.2) and so **explicitly opts out of
  the failsafe** — the ball then stays captured until a later `tb.kick`
  (10-scripting.md §3.4). That opt-out is bounded from outside by the two
  force-eject paths below; a script-held ball is never unrecoverable.
- **Tilt never suppresses capture timing (binding; 11-game-framework.md
  §5).** The `capture_ms` countdown is a sim timer: it keeps running during
  tilt and is **never** paused, suppressed, or cancelled. "Kickers and
  magnets de-energized" on tilt means exactly two things — no NEW captures
  are taken, and no scripted kicks are honoured — and nothing else. On the
  tick the framework raises `tilt` it immediately commands **every**
  CAPTURED ball to eject at its element's default `eject_speed` /
  `eject_angle_deg`: every kicker hold **including a script-held one**
  (`hold_ticks = 0`, `tb.kick_hold`), plus every `ball_lock`, which empties
  one ball per 500 ms (§6.14). Locked balls therefore release too. The same
  force-eject runs on a Duel round timeout (11-game-framework.md §3.4).
  This is what removes the deadlock: during tilt, script timers are frozen
  and cabinet-button `switch_hit`s are suppressed, so a script-held ball
  would otherwise have no release path at all and would hang the ball's
  end-of-ball wait forever.
- **The stuck-ball watchdog also overrides holds (11-game-framework.md
  §4.6).** If no ball is FREE and none is in the plunger lane for 30,000
  ticks, the framework's ball search **does** eject kickers and locks —
  script-held balls included — and logs an error. A never-released
  `tb.kick_hold` is always recovered and counted; it is never a hang.
- Eject (auto at timer expiry, or `tb.kick`): place ball at `pos`, then:
  - `"saucer"`/`"scoop"`: `v = eject_speed · (cos φ, sin φ)`,
    `φ = eject_angle_deg` (world CCW from +x), `eject_speed = 3.0 m/s`,
    mode = FREE on the kicker's layer. Run push-out (§3.8) immediately.
  - `"vuk"` (vertical up-kicker): the ball is bound to a ramp: at load,
    find the ramp whose entry node (either path end) is within 0.03 m of
    the kicker `pos`; `tb_validate` fails the table if none or multiple
    match. Eject sets `mode = RAMP` on that ramp with `s` at the matched
    end and `s_dot = eject_speed` directed into the path.
- Eject is deterministic: identical state ⇒ identical eject vector. A 100
  ms pre-eject "settle" visual state is published for animation; physics
  eject is instantaneous at the eject tick.

### 6.10 `ramp` — the 1-D constraint

#### 6.10.1 Path parametrization

The `path` nodes (segments and arcs, §3.1 baking) form a polyline-with-arcs.
At load, compute cumulative 2-D arc length; total `S`. For any `s ∈ [0, S]`:
2-D point `Γ(s)`, unit tangent `t̂(s)` (pointing toward increasing s).
`height_profile` keyframes `{s: normalized 0..1, z: meters}` define `z(u)`
piecewise-linearly in `u = s/S`; slope `dz/ds = Δz / (Δu · S)` per segment.
Default profile if absent: `[{s:0, z:0}, {s:1, z:0}]`.

```
side view:      z
                |        ____________
                |     __/            \        drop_exit: ball falls
                |  __/                \__     back to layer 0 here
                |_/                      \_↓
                +——————————————————————————— s
               s=0 (entry seam)            s=S
```

#### 6.10.2 Binding (entry)

Each path end (s = 0 and s = S) that is not flagged internal is an **entry
seam**: a segment of length `width` (default 0.044 m) centered at the end
point, perpendicular to the tangent there.

**A seam's layer is derived from the height profile at that end, never from
the ramp's `layer` field.** `layer` is the ramp's *entry* layer and is
fixed at 0 in v1 (09-table-format.md §4.21), so reading it would leave
every layer-1 end with no seam and strand balls on the upper playfield.
The rule (binding, matching 09-table-format.md §4.13's V011 tolerance):

```
seam_layer(z_end):
  if |z_end|                       <= 0.005 m : layer 0
  if |z_end − playfield.layer1_z|  <= 0.005 m : layer 1
  otherwise                                   : invalid — V011 rejects
                                                the table at load
z_end = z(0) for the s = 0 end, z(1) for the s = S end
```

A FREE ball binds only if `ball.layer == seam_layer` for that end, so a
ramp whose far end arrives at `layer1_z` is enterable from layer 1 in the
down-hill direction — this is how Atomic Diner's upper counter drains back
to the playfield (15-launch-tables.md). Both directions must work.

A FREE ball on the seam's layer binds when, during a tick, the
segment from its pre-tick to post-tick center position crosses the seam
**and**:

```
alignment: angle between v and the into-path tangent ≤ 50°
speed:     dot(v, into-path t̂) ≥ 0.1 m/s
```

On bind: `mode = RAMP`, `s = 0` (or `S`), `s_dot = dot(v, into-path t̂)`
(signed so `s_dot > 0` means toward the other end when entering at 0, and
`s_dot < 0` when entering at S). Lateral velocity is discarded (the ramp's
side guides absorb it — 5% of `|s_dot|` is additionally removed:
`s_dot ← 0.95 · s_dot`). `omega_z` is kept and keeps decaying at
`kSpinDamp`.

A ramp end may instead be flagged internal (`drop_exit`, VUK feed) — such
ends have no entry seam, and `seam_layer` is not evaluated for them (a
`drop_exit` end may sit at any profile z; the ball leaves it airborne).

#### 6.10.3 While bound

The ball is removed from the 2-D broadphase and ignores all 2-D colliders,
triggers, magnets, and nudges (ramps have walls; nudge influence on ramp
balls is negligible and skipping it keeps ramps simple). Rollover-style
scoring on ramps uses `ramp_made` and kicker/rollover elements at the exits.

#### 6.10.4 1-D dynamics

Per tick (step 3):

```
a_s = − g·sin(slope) · dot(t̂(s), ŷ)          // table-plane gravity
      − g·cos(slope) · dz/ds                  // height profile climb
      − 0.10 · s_dot                          // linear damping (s⁻¹)
      − 0.015 · g · sign(s_dot)               // ramp rolling resistance
        (rolling term only if |s_dot| > 0; never reverses sign within
         the tick: if |a_s·dt| > |s_dot| from the friction terms alone,
         s_dot ← gravity-only result)
s_dot ← s_dot + a_s·dt        ;  |s_dot| clamped to 12 m/s
s     ← s + s_dot·dt                          // step 4
pos   = Γ(clamp(s, 0, S)) ; z = z(s/S)        // step 5 projection
```

The 2-D arc length is used as-is (the 3-D correction factor
`sqrt(1+(dz/ds)²)` is deliberately ignored; state this in code comments).

#### 6.10.5 Ball-ball on the same ramp

If two balls are bound to the same ramp and `|s_i − s_j| < 2r`, resolve a
1-D equal-mass collision: exchange `s_dot` values scaled by `e = 0.95`
(`s_dot_i', s_dot_j' = mean ∓ 0.95·(mean − s_dot_i), …`), then separate to
`|s_i − s_j| = 2r` symmetrically. Balls on different ramps or a ramp/free
pair do not interact (accepted 2.5D fiction).

#### 6.10.6 Exit and fall-back

- `s > S` (forward exit): unbind. If `drop_exit: true`, the ball transfers
  to **layer 0** with `v = s_dot · t̂_2D(S)` preserved (the drop itself is
  visual; z snaps to 0) and 5% speed loss (`v ← 0.95·v`). Otherwise the
  ball becomes FREE on the **derived** exit layer, with the same velocity
  rule. Emit `switch_hit{id, ball_id, speed, tags}` then
  `ramp_made{id, ball_id}` (10-scripting.md §4.1's payload, verbatim) only
  on forward exit.
- **There is no `exit_layer` key.** The exit layer is *derived*, exactly
  like the seam layer: `seam_layer(z(1))` from §6.10.2 — layer 0 when the
  final height-profile keyframe z ≈ 0, layer 1 when it is within 0.005 m
  of `playfield.layer1_z`. Authors control it only by writing that
  keyframe (V011 rejects any other value on a non-`drop_exit` end), and
  09-table-format.md declares no such parameter.
- `s < 0` (rolled back out): unbind onto the s = 0 end's derived layer
  `seam_layer(z(0))`, `v = s_dot · t̂_2D(0)` (s_dot is negative — the ball
  exits backward out of the mouth it entered). No `ramp_made`. A ball that
  bound at the s = S end and rode the whole path down leaves here too, on
  layer 0 — the downhill traversal of a layer-1 ramp, and equally the exit
  that a `drop_exit: false` upper-counter chute relies on.
- **Fall-back-off:** if `s_dot` crosses zero while within 0.05 m of **the
  end the ball bound at** and the ball is still essentially at that end's
  height (`|z(s/S) − z_end| ≤ 0.005 m`, with `z_end` that end's profile z —
  0 for a layer-0 seam, `layer1_z` for a layer-1 seam), unbind immediately
  at the current position onto `seam_layer(z_end)` with
  `v = s_dot · t̂_2D(s)` (≈ 0). This kills bind/unbind oscillation at the
  seam for balls that never really made the ramp, in either direction.
- After any unbind, run push-out (§3.8) before the next tick's CCD.

### 6.11 Layers and transitions

- `layer` is an integer, 0 (main) or 1 (upper playfield) in v1. Every
  collider/trigger applies only to balls on its own layer (§3.1).
- Gravity, drag, and all §1.3 forces are identical on all layers.
- The **only** layer transitions are: ramp exits (§6.10.6), VUK ejects
  (§6.9), and ramp binds. There is no free-fall between layers; an upper
  playfield must be drained by ramps (typically `drop_exit`) — enforced by
  `tb_validate` reachability checks (09-table-format.md).
- **Ramps are bidirectional.** A ball may bind at either seam-bearing end,
  and each end's seam sits on the layer its profile z implies (§6.10.2), so
  the same ramp that carries a ball up to layer 1 carries it back down when
  a layer-1 ball crosses the upper seam. An upper playfield served by one
  `drop_exit: false` ramp is therefore both reachable and drainable with
  that one element — which is exactly what Atomic Diner's counter does.

### 6.12 `magnet`

- Field: while enabled and the ball (`FREE`, same layer) is within
  `radius = 0.09 m` of `pos`:
  ```
  d      = max(|ball.pos − pos|, 0.010)          // core clamp
  F      = strength · (0.05 / d)²                 // strength = 1.2 N @ 5 cm
  fade   = clamp((radius − d_true) / 0.01, 0, 1)  // last cm fades to 0
  a_mag  = min(F/m, 60 m/s²) · fade · normalize(pos − ball.pos)
  ```
  Added in step 3. The fade removes the on/off discontinuity at the rim.
- Eddy damping while in the field (enabled, `d_true ≤ radius`):
  `omega_z ← omega_z · exp(−8·dt)` (magnets kill spin) and
  `v ← v · exp(−3.5·dt)` (models induced-current braking; also what lets a
  magnet actually capture and hold a ball above `pos` — ADR-023 raised the
  0.8/s original: at that value the braking cannot dissipate the
  rim-to-rim gravity gain, so every through ball escaped and FT-09's catch
  band was unreachable).
- Control: `default_on` (false); `tb.magnet_on/off`; `tb.magnet_pulse(id,
  ms)` runs an envelope over duration T: full strength for `0.6·T`, then
  linear decay to 0 over `0.4·T`.
- Magnets are active elements (exempt from §4.4). No events emitted by the
  magnet itself; scripts use timers plus region checks.

### 6.13 `captive_ball`

A real ball permanently constrained to the slot segment `a→b` (1-D). Same
radius/mass as a normal ball; slot axis `â = normalize(b − a)`,
slot length `L_slot = |b − a|` (09-table-format.md §4.15 range
0.040–0.120 m). State is the captive's center arc position
`s_c ∈ [r, L_slot − r]` measured from `a` along `â`, plus `ṡ_c`; the
world position is `a + s_c·â` (recomputed in step 5, so drift is
impossible).

- Its own motion: 1-D with gravity `−g·sin(slope)·dot(â, ŷ)`, rolling
  resistance as §1.3, `s_c` clamped to `[r, L_slot − r]`. Both ends bounce
  with `e = 0.4` (`ṡ_c ← −0.4·ṡ_c` on the clamp).
- Free ball impact (found by the §8 swept pair test — the captive ball is
  in the pair set): resolve with the captive's admissible motion along `â`
  only. With `n̂` = contact normal (captive → free ball flipped
  appropriately), `u_n` = relative normal speed, `e = 0.9`:
  ```
  j = −(1 + e)·u_n / (1/m + (dot(n̂, â))² / m)
  v_free    += (j/m) · n̂
  v_captive += (j · dot(n̂, â) / m) · â
  ```
  If `dot(n̂, â) = 0` the captive is immovable for this contact (kinetic
  wall). On a perfectly in-line hit (`|dot(n̂, â)| = 1`) against a resting
  captive, `j = −0.95·m·u_n`: the captive leaves at `0.95 ×` the impact
  speed and the free ball keeps only `0.05 ×` its normal speed — it
  visibly "swaps momentum", the signature captive-ball feel.

**Events (binding; canon §5.7 lists both).** A captive ball produces two
different events on two different ticks, and only the first is a
`switch_hit`:

1. **The strike.** Every resolved free-ball → captive-ball impact emits the
   standard `switch_hit{id, ball_id, speed, tags}` on the tick of the
   contact, where `id` is the `captive_ball` element id, `ball_id` is the
   **striking free ball**, `speed` is that ball's speed `|v_free|` in m/s
   at the contact TOI *before* this contact's impulse is applied (the §6
   preamble definition of `speed`), and `tags` is the element's `"tags"`
   array. There is **no extra flag, field, or variant** on this payload —
   a script that wants "hard hits only" thresholds on `ev.speed` itself.
   Re-trigger debounce 100 ms per element (chatter guard, as §6.4).
2. **The full travel.** When the captive ball reaches the far end
   (`s_c` clamps at `L_slot − r`) with arrival speed `ṡ_c ≥ 0.3 m/s`, emit
   the specialized event `captive_full_travel{id}` — *before* the `e = 0.4`
   end bounce is applied, in step 5 of that tick. Arrivals below 0.3 m/s
   bounce silently. Hysteresis (pitfall 16): the far end re-arms only once
   `s_c ≤ L_slot − r − 0.004 m`. The near end `a` never emits anything.

`captive_full_travel` is the **one specialized event in §6 that is not
preceded by its own same-tick `switch_hit`** (the §6 preamble pairing rule
is otherwise universal): the travel is a delayed mechanical consequence of
a strike whose `switch_hit` already fired, so pairing it again would
double-count "any switch" frenzy scoring. Ordering is still guaranteed —
the strike resolves in step 4 and the captive projection runs in step 5, so
the `switch_hit` always precedes the `captive_full_travel` it caused. In
practice they are never even on the same tick: the shortest legal slot has
`L_slot − 2r = 0.040 − 0.027 = 0.013 m` of travel, which exceeds the
0.012 m a ball covers in one tick at the 12 m/s clamp.

Worked example (Atomic Diner's `shaker`, slot `a = (0.085, 0.560)` →
`b = (0.085, 0.640)`, i.e. `L_slot = 0.080 m` pointing straight up-table):
travel is `0.080 − 0.027 = 0.053 m` against a deceleration of
`g·sin(6.5°) + μ_rr·g·cos(6.5°) = 1.1105 + 0.2437 = 1.3542 m/s²`. An
in-line strike at 0.8 m/s starts the captive at `0.95·0.8 = 0.76 m/s` and
it arrives at `b` at `sqrt(0.76² − 2·1.3542·0.053) = 0.66 m/s` — clear of
the 0.3 m/s gate. Solving for the gate gives a minimum in-line strike of
**0.51 m/s** to emit `captive_full_travel` on that slot; weaker hits still
emit `switch_hit`. Tables that score full travel must set their design
threshold above this floor.

### 6.14 `ball_lock`

- **Capture is unconditional (this section owns it; 11-game-framework.md
  §4.4 defers here).** Circle region radius 0.02 m at `pos`; a FREE ball
  entering with any speed while `held < capacity` is captured by the sim
  (`mode = CAPTURED`, stacked bookkeeping position). Emits
  `switch_hit{id, ball_id, speed, tags}` then `ball_lock{lock_id, count}`,
  quoting 10-scripting.md §4.1 verbatim (the payload owner): `lock_id` is
  this element's id and `count` is the number of balls now held here. The
  captured ball's index is deliberately **not** in this payload — the
  preceding `switch_hit` carries `ball_id`, and lock inventory is
  observable from the snapshot.
  There is **no script confirm step** — no API to accept or decline a
  capture exists, and none ever did; any "script confirms the lock"
  wording elsewhere is wrong. A script that does not want the ball (an
  unlit lock, a lock already full in rules terms) calls
  `tb.release_lock(id, 1)` from its own `ball_lock` handler; that
  mandatory unlit-lock pattern is documented in 10-scripting.md and every
  table with a `ball_lock` must implement it.
  When `held == capacity`, the region is inert (balls roll past/over).
- **Sim failsafe: 3000 ms unclaimed auto-release.** Each captured ball
  starts a 3000 ms countdown at capture — the lock analogue of the kicker
  `capture_ms` auto-eject (§6.9), and mandatory for the same reason. The
  countdown is cancelled when the ball is either *released* (any
  `tb.release_lock` on this lock that covers it) or *claimed* (its
  `ball_lock` event reached at least one registered Lua handler that
  returned without error — i.e. the rules file demonstrably knows about
  this lock). If neither happens within 3000 ms the sim auto-releases
  exactly one ball — the oldest held — using the release kinematics below
  and logs a warning `ball_lock <id>: unclaimed ball auto-released after
  3000 ms`. A table whose rules never handle `ball_lock` therefore cannot
  swallow the machine's balls.
- Release (`tb.release_lock(id, n)`, the 3000 ms failsafe, tilt, or a ball
  search): eject one ball every 500 ms (matching `tb.release_lock`,
  10-scripting.md §3.4) until n released, each with extension params
  `eject_speed` (default 2.5 m/s) and `eject_angle_deg` (default −90° ≡
  270°, straight down-lane toward the player — 09-table-format.md §4.16
  writes it as −90); push-out after each. These eject kinematics and this
  500 ms cadence are the authoritative spec (09-table-format.md §4.16 and
  11-game-framework.md §4.4 both defer here).
- **Forced release.** On `tilt` and on a Duel round timeout the framework
  empties every lock at the same one-per-500 ms cadence, with the element
  defaults above (§6.9, 11-game-framework.md §5/§3.4). The stuck-ball
  watchdog's ball search likewise **does** eject locks
  (11-game-framework.md §4.6).
- **No lock state outlives the ball (binding).** Because releases are
  staggered at one ball per 500 ms, two framework rules make that literal:
  - A release sequence that still owes balls — forced (tilt, Duel timeout,
    ball search) or scripted (`tb.release_lock`) — **blocks end of ball**.
    It is the fifth condition on T10 (11-game-framework.md §2.2), so the
    machine cannot leave BallInPlay mid-stagger and strand the balls this
    lock has not ejected yet.
  - At end of ball, any ball still physically held in a lock is **drained
    to the trough** by the framework as part of the end-of-ball sequence
    (11-game-framework.md §4.5). That is a bookkeeping transfer, not a
    play event: the ball is not ejected onto the playfield and emits no
    `drain` (nothing crosses the outhole capsule, §6.15). So `held == 0`
    and every failsafe countdown is clear at each `ball_start`, and a
    table can never carry lock inventory across balls or players.
- `style` selects the visual treatment only (see 13-art-direction.md).
  Physical multi-ball stacking inside the lock is not simulated.

### 6.15 `outhole` and `trough`

- `outhole` region: segment `a→b`, capsule radius 0.02 m around it. A FREE
  ball on layer 0 whose center enters the capsule is drained: the ball is
  removed from play immediately (slot freed, `live = false`, trough count
  incremented) and `drain{ball_id, balls_remaining}` is emitted. **There is
  no trough-entry switch** — no outhole → trough transit is simulated, so
  this single event is the only signal that a ball left play. The framework
  decrements its balls-in-play count on that `drain` event, exactly once per
  event (11-game-framework.md §4.4), and decides what the drain means (ball
  save, next player, …).
- `trough` is pure bookkeeping: `capacity` (default 4) balls total in the
  machine. Serving a ball (`tb.add_ball`, ball_start): if trough count > 0,
  decrement it and spawn a FREE ball resting on the plunger tip:
  `pos = plunger.pos + (r + 0.002)·ĵ_lane`, `v = 0` (`ĵ_lane` from §6.16).
  Serving with an empty trough is a scripting error (logged, no-op).

### 6.16 `plunger`

- Geometry: `pos` = resting face-tip position in the plunger lane; lane
  direction `ĵ_lane = (cos φ_L, sin φ_L)`, extension param
  `launch_angle_deg` default 90 (straight up-table). The face is a static
  wall segment (width 0.03 m, material `steel`) perpendicular to `ĵ_lane`
  at `pos`, so idle balls rest on it.
- Charge: while the launch button is held with a ball in the contact zone
  (ball center within 0.04 m of `pos`), charge
  `q = min(1, held_ticks / (charge_time_s · 1000))` (default
  `charge_time_s = 1.5`, matching 01-product.md §4.4/§7; the
  09-table-format.md registry carries the same default). The plunger
  visual pull position = `q` in the snapshot.
- Release: on button release,
  ```
  v_launch = max_speed · (0.2 + 0.8 · q)        // max_speed = 7.5 m/s
  ball.v  += v_launch · ĵ_lane
  ```
  then emit `ball_launched{ball_id}` — 10-scripting.md §4.1/§4.2 owns that
  payload and it carries the ball id only; `v_launch` is **not** a payload
  field (a script that needs launch speed reads the ball's velocity from
  the snapshot on the launch tick). Releasing with no ball in the zone just
  resets the charge silently.
- **Skill-shot repeatability guarantee:** `v_launch` is a pure function of
  `held_ticks` — no randomness, no wall clock. Sensitivity at defaults is
  `7.5·0.8/1500 = 0.004 m/s per ms` of hold, so a human or autoplay script
  repeating a hold within ±10 ms repeats launch speed within ±0.04 m/s.
  A regression test asserts two runs holding exactly 700 ticks produce
  bit-identical launch velocity.
- `auto: true` (autolauncher): `auto_delay_ms` (registry default 500,
  09-table-format.md §4.4) after a ball is served into the zone, launch
  at `q = 1` regardless of input; player input is ignored.

### 6.17 `light` and `toy`

`light` has no physics. `toy` has physics only if `collider` is present:
the path bakes to static walls, material `plastic`, on the toy's layer.
Animation hooks (`anim`) are visual (13-art-direction.md).

## 7. Nudge and tilt

### 7.1 Nudge impulse

A nudge is a **30 ms half-sine acceleration envelope** applied to every FREE
ball (all layers; not RAMP or CAPTURED balls):

```
a(t) = A · sin(π · t / 0.030) · d̂ ,  t ∈ [0, 0.030] s
Δv (integral) = A · 2·0.030/π = 0.0191·A
```

Direction convention (the button names the direction the cabinet is shoved;
balls accelerate the opposite way **relative to the table**, which is what
the sim applies):

| Input | Ball acceleration direction d̂ | Δv by settings level 1/2/3 (m/s) |
|---|---|---|
| `nudge_left` (shove cab left) | `(+1, 0)` | 0.15 / 0.25 / 0.35 |
| `nudge_right` (shove cab right) | `(−1, 0)` | 0.15 / 0.25 / 0.35 |
| `nudge_up` (slap the front) | `(0, +1)` | 0.20 / 0.30 / 0.40 |

`A = Δv / 0.0191` (level 2 side nudge: A = 13.1 m/s²). The settings level
(default 2) comes from the game config (11-game-framework.md); it is part
of the replay header. Overlapping nudges sum their envelopes. Nudge inputs
are edge-triggered (a new press starts a new envelope; holding does
nothing).

### 7.2 Tilt bob and danger thresholds

**Division of labour (binding, 11-game-framework.md §5).** The sim owns
tilt-*danger detection* only: the bob, the thresholds below, and the §7.3
abuse accumulator. Whenever a danger threshold is crossed the sim pushes one
neutral `danger_threshold` sim event. **The sim never emits `tilt_warning`
or `tilt`** and never applies a tilt consequence: those are script-facing
events (canon §5.7) produced by the framework, which counts
`danger_threshold` events per ball (`game.tilt_warnings` ∈ {1,2,3},
default 2 — the first N events are warnings, the next is TILT) and performs
every consequence itself (flippers/coils off, ball save cancelled, ledger
frozen, and **every captured ball force-ejected** — §6.9/§6.14). The
framework never re-computes danger, and the sim never changes element
timing because a tilt happened: `capture_ms` and the §6.14 3000 ms failsafe
run identically before, during, and after a tilt.

Event payload (one per crossing, generated in the tick that crosses):

```
SimEvent{danger_threshold, source, magnitude, crossing_index}
  source         : BOB_WARN | BOB_HARD | ABUSE   // which threshold crossed
  magnitude      : |p| in m for the bob sources, acc in m/s for ABUSE
  crossing_index : u16, 1-based index of this crossing within the current
                   per-ball danger state (all sources share one counter;
                   reset with that state — 11-game-framework.md §4.5)
```

`source`/`magnitude` are diagnostics and presentation hints (audio, the
backglass danger flash); the framework's count treats every
`danger_threshold` alike. `crossing_index` exists so the framework can
detect a dropped or duplicated event (11-game-framework.md §5); the sim
increments it on every emission and never reuses a value within a ball.

The tilt bob is a damped 2-D oscillator integrated in the sim at tick rate:

```
state: p (m, 2-D), ṗ (m/s)
p̈ = −ω_n²·p − 2·ζ·ω_n·ṗ           ω_n = 9 rad/s, ζ = 0.15
on each nudge press: ṗ += Δv_nudge · d̂_cab   // d̂_cab = −d̂ (cab motion)
```

A single level-2 side nudge peaks `|p| ≈ 0.026 m`; the envelope decays as
`exp(−1.35·t)` (≈ 26% left after 1 s).

Thresholds (per-table overridable under the `physics.tilt` sub-object of
the §1.3 override set; **09-table-format.md §2 is the schema owner** of the
keys, ranges and defaults):

| Constant | `physics.tilt` key | Default | Meaning |
|---|---|---|---|
| bob warn | `warn` | 0.055 m | upward crossing emits `danger_threshold{BOB_WARN}` |
| bob hard | `hard` | 0.085 m | upward crossing emits `danger_threshold{BOB_HARD}` |
| abuse | `abuse` | 1.2 m/s | §7.3 accumulator crossing emits `danger_threshold{ABUSE}` |
| re-arm | — (derived) | 0.7·threshold | that threshold re-arms below this; not authorable |

Each bob threshold is independently armed: it emits on an upward crossing
and re-arms only once `|p|` falls below `0.7 ×` its own value (0.0385 m and
0.0595 m at defaults), so one slosh cannot machine-gun events. A hard slam
that sweeps past both bob values emits two events, one per threshold, in
threshold order within the tick (the `abuse` threshold is the separate §7.3
source, armed the same way). The bob state `p`, `ṗ` and both arm latches
are published in the snapshot.

### 7.3 Anti-abuse accumulator

A leaky integrator, also published in the snapshot:

```
on each nudge press: acc += Δv_nudge        // m/s
every tick:          acc ← max(0, acc − 0.15·dt)   // leaks 0.15 m/s per s
on acc crossing physics.tilt.abuse (default 1.2) upward (armed):
    emit danger_threshold{source=ABUSE, magnitude=acc}
    re-arms when acc < 0.7·1.2 = 0.84
```

The threshold is the third and last `physics.tilt` key (§1.3, §7.2);
the 0.7 re-arm fraction and the 0.15 m/s leak rate are not authorable.

This catches rapid alternating nudges that a symmetric bob would cancel. It
is a third danger source, not a shortcut to tilt: like the bob crossings it
emits `danger_threshold` and the framework decides whether that event is a
warning or the tilt.

Both the bob state and `acc` are replayed state (nudges are inputs). The
framework resets all danger state — `p`, `ṗ`, `acc`, and all three arm
latches — at every end of ball (11-game-framework.md §4.5 step 5), so
danger is strictly per ball; the sim exposes that reset as a plain command,
never as a wall-clock or per-game timer.

## 8. Multiball: ball-ball collisions

- **Swept pair test** (used in §3.6): relative motion
  `w(t) = (p_j − p_i) + (v_j − v_i)·t`; solve `|w(t)| = 2r` via the §3.3
  quadratic with `ρ = 2r`. Approach condition `dot(w, v_j − v_i) < 0`.
  Both balls must be FREE and on the same layer. The captive ball (§6.13)
  participates with its constrained response.
- **Resolution:** equal masses. Normal `n̂ = normalize(p_i − p_j)` (points
  toward ball i). Relative contact velocity includes both spins:
  `u = (v_i − r·ω_i·perp(n̂)) − (v_j + r·ω_j·perp(n̂))` — note the sign:
  each ball's contact offset is toward the other ball. With
  `u_n = dot(u, n̂) < 0`:
  ```
  e = 0.93 (steel-steel), through the §4.2 curve
  j_n = −(1 + e_eff)·u_n · (m/2)          // reduced mass m/2
  j_stick = −u_t · (m/2) / (1 + 2·(r²·m/2)/I)  … simplified: use
  j_t = clamp(−(m/7)·u_t, ±μ·j_n)         // μ = 0.05, both spheres spin
  v_i += (j_n·n̂ + j_t·t̂)/m ;  v_j −= (j_n·n̂ + j_t·t̂)/m
  ω_i += κ·(−r·j_t)/I      ;  ω_j += κ·(−r·j_t)/I     // κ = 0.20
  ```
  (Tangential effective mass for two free spheres is `m/7`: each
  contributes `2/(7m)⁻¹` in series. Both spins change with the same sign
  because the contact offsets are opposite.)
- Ball-ball contacts count against **both** balls' `kMaxToiIter` budgets.
- With ≤ 6 live balls this is at most 15 pair tests per CCD iteration; no
  broadphase, no sorting beyond the fixed (i < j) index order.
- Multiball spawn (`tb.add_ball`): serves from the trough (§6.15) or ejects
  from a lock (§6.14); balls never spawn overlapping — the serve position
  is occupied-checked and the serve is deferred (tick-retried) while
  another ball is within 2r + kSkin of it.

## 9. Performance

- **Budget:** ≤ 50 µs **mean** per tick with 4 FREE balls in play, and
  ≤ 200 µs p99, measured on the reference cabinet PC (canon §1). At 1000 Hz
  this is ≤ 5% of one core.
- **Instrumentation:** `SimTickStats { t_broadphase, t_toi, t_resolve,
  t_triggers, t_total, toi_iters, frozen }` per tick, aggregated over a
  rolling 10 000-tick window (mean, p99), published in the snapshot and
  shown in the timing overlay (05-engine-core.md). Timing instrumentation
  reads the monotonic clock **outside** the deterministic state — timings
  are diagnostics, never inputs.
- **CI gate:** 16-testing-ci.md §2.9 owns the harness; this section owns
  the numbers. It is a ladder, not one test — 4 balls, median-of-3 runs,
  the same limits at every rung (**mean < 100 µs, p99 < 200 µs** on CI
  runners, a deliberate 2× margin over the reference budget for noisy
  shared hardware):
  - **M2** — `perf_tick.gate_synthetic`, a programmatically built collider
    scene (no `table.json`, no Lua), so the harness itself is proven before
    any table exists.
  - **M5+** — the same harness gains `test-lab` and each shipped table as
    it lands.
  - **M9** — the harness starts loading `rules.lua` with the table.

  The 50 µs mean figure is verified manually on reference hardware at M19;
  it is not a CI number.
- **If the budget is exceeded**, in order: (1) verify broadphase health —
  cells should average < 8 colliders; consider cell 0.032 → 0.048 m;
  (2) confirm candidate buffers and stamp arrays are reused, zero
  allocations per tick (assert in debug builds); (3) check conservative-
  advancement iteration counts (mean < 3; if higher, the ω fast-path
  threshold is wrong); (4) profile before touching math. **Never** lower
  the tick rate, skip CCD for "slow" balls, or cap the resolution loop
  below 8 — those trade correctness for speed and are rejected in review.

## 10. References

Study **Visual Pinball X** (open source) to understand these concepts, then
implement them from this spec. **Do not copy code or constants**: VPX's
license is incompatible with Tiltburst's public-domain (Unlicense) release,
and its solver structure differs from this spec anyway. Worth studying:

- Flipper solenoid modeling: ramp-up torque, end-of-stroke behavior, and
  why live catch needs explicit handling.
- `elasticityFalloff`: velocity-dependent restitution as the single biggest
  contributor to believable bounces (§4.2 is our formulation).
- Their hit-time (TOI) loop: earliest-contact iteration within a physics
  step, and the bugs their comments warn about.
- Flipper collision as a moving surface with contact-radius-dependent
  velocity transfer (§5.3 is our formulation).
- Nudge/tilt-bob modeling as a spring-damper driven by input.

Secondary: Coulomb impulse friction for spheres (any real-time physics
text, e.g. the classic Baraff course notes) for §4.1's effective-mass
derivations.

## Common pitfalls

1. **Tunneling via discrete tests.** Moving a ball then checking overlap
   loses contacts at > ~3 m/s against thin walls. Correct: every
   ball-collider interaction goes through the swept tests (§3.2–3.5);
   position updates happen only inside the CCD loop (§3.6).
2. **Tunneling through corners.** Testing only segments (not their endpoint
   caps) lets balls cut corners between two walls. Correct: bake point
   colliders at every node and arc endpoint exactly once (§3.1).
3. **Tunneling by the flipper sweeping over the ball.** Testing the flipper
   only at its start-of-tick pose misses contacts mid-rotation (tip moves
   3.2 mm/tick at ω_max). Correct: conservative advancement against the
   rotating capsule whenever `|ω| ≥ 1 rad/s` (§3.5).
4. **Broadphase misses grazing contacts.** Forgetting to inflate baked
   collider AABBs (and the query AABB) by ball radius + skin drops real
   TOIs at cell boundaries. Correct: §3.7 inflation on both sides.
5. **Energy injection from push-out.** Adding velocity along the
   depenetration normal, or resolving penetration with restitution, pumps
   energy and makes balls vibrate then explode. Correct: push-out is
   position-only (§3.8); impulses only ever apply at swept TOIs with
   `u_n < 0`.
6. **Applying restitution to separating contacts.** Skipping the
   `u_n >= 0 → return` guard (§4.1) reverses outgoing balls and doubles
   energy. The §4.4 property test exists to catch exactly this class.
7. **Re-resolving the same contact every iteration.** Without the kSkin
   pull-back (§3.6) the same surface reports TOI = 0 forever, eating the
   iteration budget and freezing balls mid-air. Correct: maintain kSkin
   separation after every resolution.
8. **Resting-contact jitter.** Full restitution at millimeter-per-second
   impacts makes cradled balls buzz. Correct: `kRestSpeed` cutoff (§4.2)
   plus the FT-03 acceptance test.
9. **Non-determinism from containers and clocks.** `unordered_map`
   iteration order, pointer-keyed sorts, wall-clock reads, or any RNG
   beyond the two §2.2 streams silently break replays. Correct: §2.2
   rules; the replay test in 16-testing-ci.md runs in CI on every PR.
10. **Fast-math flags.** `-ffast-math` (or `/fp:fast`) lets the compiler
    reorder float ops differently per build and breaks replay files against
    rebuilt binaries. Correct: §2.2 rule 2 flags, set on `tb_sim`
    explicitly in CMake.
11. **Flipper-feel dead end: static flipper + fake "flip impulse".**
    Applying a canned impulse on button press instead of surface-velocity
    transfer makes every hit identical — no tip/base power difference, no
    backhands, no passes. Correct: §5.3; power differences must emerge
    from `|V_s| = ω·ρ`.
12. **Flipper-feel dead end: constant-e bounces.** Without the §4.2
    falloff, fast balls bounce off flippers uncontrollably and dead
    bounces fail; tuning e alone cannot fix both ends. Correct: keep e
    high and let the falloff curve handle high speeds.
13. **Flipper-feel dead end: springy hold.** Adding compliance to HOLD
    makes cradles wobble and FT-03 unpassable. Correct: HOLD is rigid
    (§5.2).
14. **Ramp seam oscillation.** Bind/unbind cycling at the entry (ball
    re-crosses the seam every tick) gains or loses energy each cycle.
    Correct: fall-back-off rule (§6.10.6) plus binding only on directed
    seam crossing (§6.10.2).
15. **Events at render rate.** Emitting or timestamping events from the
    render thread quantizes gameplay/audio to frames. Correct: all events
    are *generated* in the tick's phase 1 and *dispatched* in step 7
    (phases 2–4), carrying tick numbers; audio schedules sample-accurately
    from them (canon §5.4).
16. **Trigger multi-fire.** Region triggers without hysteresis fire every
    tick the ball sits in them. Correct: §6.8-style enter/exit arming on
    every region trigger (rollover, outhole, kicker, lock, gate pass, the
    captive-ball far end §6.13), and on the §7.2/§7.3 danger thresholds
    (arm latches, not level tests).
17. **The sim deciding tilt.** Emitting `tilt_warning`/`tilt` from the sim,
    or killing flippers there, duplicates policy the framework owns and
    makes the warning count unsettable. Correct: the sim emits only
    `danger_threshold` (§7.2); the framework counts and acts
    (11-game-framework.md §5).
18. **Freezing element timers during tilt.** Pausing `capture_ms`, or
    refusing to eject captured balls while `tilted`, deadlocks the machine:
    script timers are frozen and cabinet-button switches suppressed during
    tilt, so a `tb.kick_hold` ball would have no release path and the ball
    would never end. Correct: `capture_ms` runs unchanged during tilt, and
    tilt force-ejects **every** captured ball, script-held ones included
    (§6.9, §6.14).
19. **Reading a ramp seam's layer from the element's `layer` field.**
    `layer` is the ramp's *entry* layer, fixed at 0 in v1, so a ramp end
    arriving at `layer1_z` would have no seam and layer-1 balls would have
    nothing to bind to — an upper playfield with no way down. Correct:
    `seam_layer(z_end)` (§6.10.2), and the exit layer is derived the same
    way; `exit_layer` is not an authored key (§6.10.6).
20. **Pairing `captive_full_travel` with a `switch_hit`.** The strike
    already emitted one; pairing the far-end arrival too double-counts
    "any switch" scoring, and firing the strike switch only at the far end
    loses every partial hit. Correct: `switch_hit` on the impact tick,
    `captive_full_travel` alone on the arrival tick (§6.13).
21. **Inventing a lock confirm step.** Waiting for a script to accept a
    `ball_lock` capture (or holding the ball "pending") strands balls in
    tables whose rules ignore the event. Correct: capture is unconditional,
    the script declines by calling `tb.release_lock(id, 1)`, and the
    3000 ms unclaimed auto-release backstops both (§6.14).
22. **Paraphrasing event payload field names.** Emitting
    `ball_lock{id, ball, held}`, `ball_launched{ball, v_launch}` or
    `ramp_made{id, ball}` because those names read better in sim code
    breaks every rules file: 10-scripting.md §4.1/§4.2 owns the payloads
    and its field names *are* the API — `ball_lock{lock_id, count}`,
    `ball_launched{ball_id}`, `ramp_made{id, ball_id}`,
    `rollover{id, ball_id}`, `target_down{bank_id, target_index}`.
    Correct: quote the catalog verbatim, and read any extra sim-side value
    a script might want (launch speed, lock occupancy) from the snapshot
    instead of bolting a field onto the payload (§6 preamble).

## Done when

- [ ] `tb_sim` builds headless, depends only on `tb_core`, no
      render/platform/audio includes (checked by CI include-guard test).
- [ ] Determinism: a 10⁶-tick recorded-input replay (4 balls, flippers,
      nudges, all element types) reproduces every ball's `pos`, `vel`,
      `omega_z` bit-for-bit across 5 consecutive runs of the same binary,
      and the identical per-tick event sequence — sim emission order
      (phase 2) and framework emission order (phase 3) included (§2.2
      rule 7).
- [ ] Tunneling stress: 10⁵ random launches at 12 m/s against a wall grid
      with 2 mm gaps and 0.008 m posts — zero balls escape the play area
      over 10³ ticks each (ARCHITECTURE.md goal exceeded: 12 > 8 m/s).
- [ ] Energy property test (§4.4): 10⁶ randomized passive impacts, zero
      violations of `KE_after ≤ KE_before + 1e-9 J`.
- [ ] Restitution curve unit test matches §4.2 acceptance numbers ±0.01.
- [ ] FT-01 … FT-10 all pass with the bands in §5.7 unmodified
      (FT-01…FT-08 gate M4; FT-09/FT-10 gate M8, 04-milestones.md).
- [ ] Every element type in §6 has: a unit test of its trigger condition
      and response formula, and emits exactly the canonical events listed
      (verified against the canon §5.7 event list) with payload field names
      identical to 10-scripting.md §4.1/§4.2 — `ball_lock{lock_id, count}`,
      `ball_launched{ball_id}`, `ramp_made{id, ball_id}`,
      `rollover{id, ball_id}`, `target_down{bank_id, target_index}`
      included.
- [ ] Ramp integration test (promoted to FT-10, §5.7): ball at 4 m/s
      makes the 0.6 m rig ramp climbing to z = 0.08 m and exits with
      `ramp_made`; the same ball at 1.2 m/s rolls back and exits the
      entry without `ramp_made` and without ever binding twice in one
      approach.
- [ ] Ramp bidirectionality (§6.10.2/§6.10.6): on a `drop_exit: false`
      ramp whose final keyframe z = `playfield.layer1_z`, a FREE ball on
      **layer 1** crossing the far seam binds, rides down, and unbinds on
      layer 0 at the s = 0 end; the derived exit layer of the forward
      direction is 1 (no `exit_layer` key exists anywhere in the loader or
      the schema).
- [ ] Captive ball (§6.13): a strike emits `switch_hit{id, ball_id, speed,
      tags}` with `speed` = the striking ball's pre-impulse `|v|` (±0.01
      m/s) on the impact tick; a strike hard enough to send the captive to
      the far end at ≥ 0.3 m/s emits exactly one `captive_full_travel{id}`
      on a **later** tick, with no second `switch_hit`; a weak strike emits
      the `switch_hit` and no `captive_full_travel`; the far end re-arms
      only after 0.004 m of retreat.
- [ ] Lock lifecycle (§6.14): capture happens with no script involvement
      and emits `switch_hit{id, ball_id, speed, tags}` then
      `ball_lock{lock_id, count}`; `tb.release_lock(id, 3)` ejects one ball
      per 500 ms ±1 tick; a capture that is neither released nor claimed
      auto-releases one ball and logs a warning at 3000 ms; a release
      sequence still owing balls blocks end of ball (T10,
      11-game-framework.md §2.2) and any ball still locked at end of ball
      is drained to the trough, so `held == 0` at every `ball_start`.
- [ ] Tilt/timeout force-eject (§6.9/§6.14): with one ball script-held by
      `tb.kick_hold` and a lock at capacity, raising `tilt` ejects the
      kicker ball on that tick and empties the lock one ball per 500 ms;
      a separate assertion shows `capture_ms` countdowns are never paused
      while `tilted`.
- [ ] Plunger repeatability test: identical hold tick-counts give
      bit-identical launch speeds; ±10 ms hold difference gives ≤ 0.04 m/s
      speed difference (§6.16 sensitivity at defaults).
- [ ] Nudge/danger test: three scripted level-2 nudges within 1.5 s emit
      ≥ 1 `danger_threshold` (`BOB_WARN`); a single nudge emits none; 6
      alternating nudges in 2 s emit a `danger_threshold` (`ABUSE`); each
      threshold re-arms only below `0.7 ×` its value. The sim emits **no**
      `tilt_warning` and **no** `tilt` in any run — the framework produces
      those from the event count (11-game-framework.md §5, tested there).
- [ ] Ball-ball test: head-on equal-speed collision swaps velocities to
      within `e = 0.93` scaling ±1%; swept pair test passes a 10 m/s
      crossing-paths scenario with no overlap ever exceeding 0.1 mm.
- [ ] Perf: the 16-testing-ci.md §2.9 ladder is green at its current rung
      (M2 `perf_tick.gate_synthetic`; M5+ test-lab and each shipped table;
      M9 onward with `rules.lua` loaded) — median-of-3 mean < 100 µs/tick
      and p99 < 200 µs with 4 balls; zero per-tick heap allocations (debug
      assert).
- [ ] All constants in this document appear in code as named constants
      with values exactly as specified (spot-checked in review against
      §1.4, §4.3, §5.5, §7), and every key of the §1.3 `physics.*` override
      table — `rolling_resistance`, `restitution_falloff`,
      `restitution_soft`, `live_catch_window_ms`, `live_catch_factor`,
      `tilt.{warn,hard,abuse}` — is loaded from `table.json`
      (09-table-format.md §2) and demonstrably changes the matching
      constant; no other `physics.*` key exists.
