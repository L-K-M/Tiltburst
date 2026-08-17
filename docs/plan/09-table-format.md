# 09 — Table Format (table.json)

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 08-physics.md (element physics; shares this doc's parameter
registry), 10-scripting.md (event payloads, `tb.*` API), 13-art-direction.md
(art.json, palettes, `art_ref`), 12-audio.md (audio.json).

This document is the binding specification of `table.json`: the geometry,
elements, materials, and prefab instances of a table. It is written for two
readers: the implementor of `tb_table` (loader, prefab expansion, validator)
and any LLM or human authoring a new table. Element physics behavior lives in
08-physics.md; this doc owns the JSON surface. The two docs use one shared
parameter registry — a name that appears here is the name in 08-physics.md.

## 1. Table pack layout and JSON conventions

A table is a directory `tables/<slug>/` (canon 5.5):

| File | Contents | Spec |
|---|---|---|
| `table.json` | Geometry, elements, materials, prefab instances | this doc |
| `rules.lua` | Game rules script | 10-scripting.md |
| `art.json` | TBArt vector art layers and palettes | 13-art-direction.md |
| `audio.json` | SFX patches and tracker music | 12-audio.md |
| `assets/` (optional) | PNG decals / WAV sounds for human authors | 13, 12 |

JSON conventions (binding for all four JSON files):

- Strict JSON plus `//` line comments. The loader must parse with
  `nlohmann::json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/true,
  /*ignore_comments=*/true)`. No trailing commas, no `/* */` blocks, no
  NaN/Infinity. Encoding is UTF-8 without BOM.
- Lengths are **meters**, masses kilograms, times seconds unless a key carries
  a unit suffix (`_ms`, `_s`, `_deg`). Angles in JSON are **degrees**, suffix
  `_deg`, measured **counterclockwise from +x** in table space, normalized by
  the loader into [-180, 180). Internally everything converts to SI radians
  (canon 5.3).
- Table space (canon 5.3): origin at the bottom-left corner of the play area
  as the player sees it; +x right, +y up-table (away from the player); z is
  height above the playfield surface, used only by ramps and layers.
- Every element id is unique, `snake_case`, matching `^[a-z][a-z0-9_]{0,47}$`.
  Ids are the handle scripts and art use; never reuse or auto-renumber them.
- `tags` is an array of snake_case strings. Scripts can address elements by
  tag (10-scripting.md). Prefab children inherit the prefab instance's tags.
- Unknown keys anywhere in `table.json` produce validator warning V026
  (typo protection) and are otherwise ignored by the loader.

```
        +y (up-table, away from player)
        ^
 1.04 m |  ___________
        | /           \      <- rounded top corners (arcs)
        | |           | |
        | |  playfield| |    <- shooter lane on right edge
        | |           |*|
        | |  /\   /\  |*|
        | |_/  \_/  \_|*|    <- flippers, drain at bottom center
  0.00  +----------------> +x
       0.00            0.52 m
```

## 2. Top-level schema

```json
{
  // table.json — annotated skeleton. All keys shown; optional keys carry
  // their default values.
  "format_version": 1,                    // integer, required. Currently 1.
  "meta": {
    "slug": "my-table",                   // required; must equal directory name
    "name": "My Table",                   // required; display name, <= 32 chars
    "theme": "haunted lighthouse",        // required; free text
    "author": "somebody",                 // required
    "description": "One-paragraph blurb.",// required; <= 600 chars
    "rules_card": "Hit 3 banks to start Storm Mode. Left ramp relights ball save.",
                                          // required; <= 400 chars; shown in
                                          // attract mode as the rule card
    "replay_score": 5000000,              // optional; replay award threshold
                                          // (11-game-framework.md 3.3)
    "default_scores": [                   // optional; exactly 10 seeded high-score
      { "initials": "TBT", "score": 5000000 }   // entries, non-increasing (11 s7)
      // ... 9 more {initials, score} entries ...
    ],
    "autoplay_bounds": {                  // optional; tb_autoplay CI gates keyed
                                          // by report metric path (grammar below)
      "ball_time_s.p50":       { "min": 25,   "max": 60,   "skill": 1 },
      "shots[left_ramp].rate": { "min": 0.25, "max": 0.55, "skill": 2 }
    }
  },
  "playfield": {
    "size": [0.52, 1.04],                 // [w, h] meters; default shown
    "slope_deg": 6.5,                     // effective slope; default 6.5
    "ball_count": 4,                      // physical balls; max in play at once
    "layer1_z": 0.055                     // height of layer 1 surface, meters
  },
  "physics": {                            // optional; per-table overrides of
                                          // 08-physics.md globals. Whole block
                                          // optional, every key optional, and
                                          // the key set is CLOSED (table below).
    "rolling_resistance": 0.025,          // mu_rr, dimensionless (08 s1.3)
    "restitution_falloff": 0.12,          // s/m, kFalloff (08 s4.2)
    "restitution_soft": 0.5,              // m/s, kSoft (08 s4.2)
    "live_catch_window_ms": 50,           // ms, kLiveCatchWindow (08 s5.4)
    "live_catch_factor": 0.15,            // scale, kLiveCatchFactor (08 s5.4)
    "tilt": {                             // sub-object, not dotted keys
      "warn": 0.055,                      // m, bob warn threshold (08 s7.2)
      "hard": 0.085,                      // m, bob tilt threshold (08 s7.2)
      "abuse": 1.2                        // m/s, abuse accumulator (08 s7.3)
    }
  },
  "materials": {                          // optional; override 08-physics.md 4.3
                                          // rows. Values below quote that table
                                          // (e, mu_s, mu_k, kappa) — it owns them.
    "wood":    { "restitution": 0.30, "friction_static": 0.25,
                 "friction_kinetic": 0.15, "spin_transfer": 0.60 },
    "steel":   { "restitution": 0.45, "friction_static": 0.15,
                 "friction_kinetic": 0.10, "spin_transfer": 0.50 },
    "rubber":  { "restitution": 0.75, "friction_static": 0.60,
                 "friction_kinetic": 0.45, "spin_transfer": 0.90 },
    "plastic": { "restitution": 0.35, "friction_static": 0.20,
                 "friction_kinetic": 0.12, "spin_transfer": 0.40 }
  },
  "light_groups": {                       // optional; script-addressable lamp groups
    "grp_top_lanes": ["light_lane_a", "light_lane_b"]  // member order = chase order
  },
  "rest_zones": [                         // optional; spots where a stopped ball
    { "pos": [0.300, 0.640], "radius": 0.020, "layer": 0 } // is legitimate (11 s4.6)
  ],
  "elements": [ /* element objects, section 4 */ ],
  "prefabs":  [ /* prefab instances, section 5 */ ]
}
```

Rules:

- `format_version` other than 1 is rejected with V022 (section 9).
- `playfield.size`: hard range w ∈ [0.40, 0.70], h ∈ [0.80, 1.40]. All shipped
  tables use the default `[0.52, 1.04]`; prefab defaults below are tuned to it.
- `slope_deg` hard range [4.0, 8.0]; recommended band [5.5, 7.0] (outside the
  band → V020 warning). Physics meaning in 08-physics.md (gravity projection).
- `ball_count` ∈ [1, 6]. The trough must hold at least this many (V009).
- `layer1_z` ∈ [0.040, 0.090]. Height of the layer-1 playing surface; ramps
  that connect to layer 1 must end at this z (V011).
- `physics`: optional block of per-table physics overrides — the block
  08-physics.md §1.3 reads when it says "per-table override
  `physics.rolling_resistance`", and the same block every other `physics.*`
  name in that document refers to. **This section owns the key set, the
  defaults, and the ranges; 08-physics.md owns what each number means.** v1
  defines exactly these keys — five scalars plus the `tilt` sub-object,
  eight authorable numbers in all:

  | Key | Type | Unit | Default | Hard range (V019) | Meaning owned by |
  |---|---|---|---|---|---|
  | `rolling_resistance` | number | — (μ_rr) | `0.025` | 0.005–0.060 | 08 §1.3 |
  | `restitution_falloff` | number | s/m | `0.12` | 0.00–0.40 | 08 §4.2 |
  | `restitution_soft` | number | m/s | `0.5` | 0.10–2.00 | 08 §4.2 |
  | `live_catch_window_ms` | number | ms | `50` | 30–80 | 08 §5.4 |
  | `live_catch_factor` | number | scale | `0.15` | 0.05–0.30 | 08 §5.4 |
  | `tilt.warn` | number | m | `0.055` | 0.030–0.120 | 08 §7.2 |
  | `tilt.hard` | number | m | `0.085` | 0.045–0.150 | 08 §7.2 |
  | `tilt.abuse` | number | m/s | `1.2` | 0.60–3.00 | 08 §7.3 |

  Per key:

  - `rolling_resistance` = μ_rr in `a_rr = −μ_rr·g·cos(slope)·v̂`;
    recommended band [0.018, 0.035] (V020). Lower = a glassy, fast,
    freshly-waxed playfield; higher = a slow, worn one.
  - `restitution_falloff` / `restitution_soft` are 08-physics.md §4.2's
    `kFalloff` / `kSoft` in `e_eff = e / (1 + falloff·max(0, s − soft))`:
    bounces stay fully elastic below `soft` and flatten above it. At the
    defaults a surface's `e` halves at approach speed
    `soft + 1/falloff = 0.5 + 8.333 = 8.83 m/s`. `falloff: 0` disables
    falloff entirely; `soft`'s 0.10 floor keeps it clear of the global
    `kRestSpeed` (0.03 m/s) below which restitution is 0 regardless.
  - `live_catch_window_ms` / `live_catch_factor` are §5.4's
    `kLiveCatchWindow` and `kLiveCatchFactor`. **The window is milliseconds
    here** (default `50`) where §5.4 writes 0.050 s — the `_ms` suffix is
    the unit, and the 30–80 range is §5.5's 0.03–0.08 s. A wider window or
    a lower factor makes live catches easier.
  - `tilt` is a **sub-object**, not dotted keys: write
    `"tilt": { "warn": 0.055 }`, never `"tilt.warn": 0.055` (that spelling
    is an unknown key, V026). `warn` and `hard` are the bob displacement
    thresholds of 08-physics.md §7.2 — the rows it names `warn` and `tilt`,
    emitting `danger_threshold{BOB_WARN}` and `{BOB_HARD}` — and `abuse` is
    the §7.3 accumulator threshold (`ABUSE`). `tilt.warn < tilt.hard` is
    required (V019). The 0.7× re-arm factor is derived from each threshold
    and is **not** authorable. Two anchors that explain the floors: a
    single level-2 side nudge peaks the bob at `|p| ≈ 0.026 m` (08 §7.2),
    so `warn`'s 0.030 m floor is where a table may make tilt harsh but
    never so harsh that one nudge at the default nudge level warns. And
    five level-2 side nudges (0.25 m/s each ⇒ 1.25 m/s) clear `abuse` 1.2
    by only 0.05 m/s, which the 0.15 m/s-per-second leak burns in 0.33 s —
    so five nudges trip ABUSE only when they land inside a third of a
    second, while six (1.5 m/s, 2.0 s of margin) trip it comfortably.

  The block and every key in it are optional and **known**, so writing them
  never trips V026; omitting a key uses the 08-physics.md default. Any
  *other* name inside `physics` — including any `physics.*` name
  08-physics.md mentions that the table above does not list — is an unknown
  key (V026, ignored) and is therefore **not authorable**: it stays a global
  constant, tunable only in code. Every value here is range-checked by V019
  (a sub-key reports as `physics.tilt.warn`). Changing any of them changes
  ball behavior table-wide — re-run `tb_autoplay` and retune
  `meta.autoplay_bounds` after touching one.
- `materials` only re-tunes the canonical material rows of 08-physics.md §4.3:
  `wood`, `steel`, `rubber`, `plastic` (assignable to elements), plus
  `flipper_rubber` (flippers only — applied implicitly, never assignable).
  Other names are rejected (V021). Per-row fields, each optional and merged
  over the 08-physics.md defaults: `restitution` (= e), `friction_static`
  (= μ_s), `friction_kinetic` (= μ_k), `spin_transfer` (= κ). **08-physics.md
  §4.3 owns the values and their meaning**; the skeleton above quotes it.
  There is no combined `friction` field — write both `friction_static` and
  `friction_kinetic` (a `friction` key in a material row is an unknown key,
  V026). Ranges: restitution ∈ [0.0, 0.95], friction_static and
  friction_kinetic ∈ [0.0, 1.0], spin_transfer ∈ [0.0, 1.0].
- `meta.replay_score`: optional positive integer, default `5000000`; the
  replay award threshold, at most the 9,999,999,999 ledger cap. Consumed by
  11-game-framework.md §3.3; shipped values in 15-launch-tables.md.
- `meta.default_scores`: optional; when present, **exactly 10** entries
  `{initials, score}` seeding the high-score table (rank 1 = Grand
  Champion): `initials` is exactly 3 glyphs from `A–Z 0–9 space`, `score` a
  positive integer, non-increasing down the array (else V028). **When absent
  the table's high-score list simply starts empty** — there is no built-in
  ladder anywhere in the framework to fall back to (11-game-framework.md §7),
  so every one of the first ten games qualifies for an entry. All five
  shipped tables declare the key; their themed values are in
  15-launch-tables.md. test-lab (section 7) deliberately omits it.
- `meta.autoplay_bounds`: optional map from a **metric path** in the
  tb_autoplay report (14-authoring-guide.md §8.2) to a bound object
  `{min, max, skill}`. `min`/`max` are numbers, each optional but at least
  one required, with `min <= max`; `skill` is **required** and is the
  tb_autoplay skill level (`0`, `1`, or `2`) the bound is measured at — a
  bound without a skill is meaningless, since every metric moves with skill.
  Malformed entries, unknown paths, and a missing `skill` are V029 errors.

  Metric-path grammar (binding; V029 validates against exactly this):

  ```
  metric_path  := dotted_path | shot_path
  dotted_path  := name ( "." name ){0,2}    // 1-3 segments, no deeper
  shot_path    := "shots[" shot_id "]." shot_field
  name         := ^[a-z][a-z0-9_]*$         // report keys: ball_time_s, p50, …
  shot_id      := element-id syntax (section 1), the shot's report `id`
  shot_field   := "attempts" | "made" | "rate"
  ```

  `dotted_path` walks the report object by key (`"stuck_balls"`,
  `"ball_time_s.p50"`, `"drains.center"`, `"coverage.share"`,
  `"score.p50"`, `"modes.wizard_reach_share"`). `shot_path` is the one
  indexed form: the report's `shots` is an **array**, so
  `"shots[left_ramp].rate"` selects the entry whose `id` is `left_ramp`
  (a path naming a shot id that no labeled shot uses is V029). Array-valued
  metrics with no id key (`score.per_ball_p50`, `coverage.missed`) are not
  boundable, and **derived quantities are not expressible** — the skill-0
  vs skill-2 score-spread ratio and the 15 % rule of
  14-authoring-guide.md §8.3 stay review-only targets, never bounds.

  Which CI job gates which bound (16-testing-ci.md §2.8 owns the runs):

  | Group | Paths | Gated by |
  |---|---|---|
  | Session-shape independent | `stuck_balls`, `script_errors`, `tilts`, `ball_time_s.*`, `drains.*`, `coverage.*`, `flips_per_ball` | the §2.8 smoke run, `--seconds 300` |
  | Full-game shape only | `score.*`, `modes.*`, `shots[<id>].*`, `tilt_warnings_per_game`, `ball_saves_used_per_game` | a separate `--balls 3` CI job |

  `tb_autoplay --check-bounds` checks only the bounds of the group matching
  the session shape it just ran and ignores the other group — a `--seconds`
  session never fails on a `score.p50` bound, and vice versa. Undeclared
  metrics are not gated (the 14-authoring-guide.md §8.3 ranges remain the
  authoring targets). A declared bound must **narrow** the §8.3 green range
  for that metric, never widen or contradict it.
- `light_groups`: optional map from a group id (element-id syntax; `grp_`
  prefix recommended, as in 10-scripting.md) to an array of `light` element
  ids. Scripts address a group exactly like a single light (`tb.light_on`,
  `tb.light_blink(id, "chase")` — 10-scripting.md §3.2); the `chase`
  pattern advances in member array order (declaration order). Members must
  exist, be `light` elements, and be unique within the group; a group id
  must not collide with any element id or another group id (V030).
- `rest_zones`: optional array of `{pos, radius, layer}` circles where a
  stationary ball is legitimate (a saucer dish, a dead-end pocket).
  Stuck-ball recovery (11-game-framework.md §4.6) never fires on a ball
  inside one. `radius` default `0.020`, range [0.014, 0.100]; `layer`
  default `0`; `pos` must be in bounds (V004). Kicker holds, ball locks,
  and the plunger lane are implicit rest zones — do not declare them.
- `elements` and `prefabs` may each be empty arrays but must be present;
  `light_groups` and `rest_zones` are optional and default to empty.

Fields common to **every** element object:

| Field | Type | Required / default | Meaning |
|---|---|---|---|
| `id` | string | required | unique snake_case id |
| `type` | string | required | one of the 20 canon types (canon 5.6) |
| `layer` | integer | `0` | `0` = main playfield, `1` = upper (section 4.21) |
| `tags` | string[] | `[]` | script-addressable labels |

## 3. Geometry paths

Several elements (`wall`, `ramp`, `toy.collider`) take a `path`: an array of
**nodes**. A node is either a point or an arc:

```json
"path": [
  [0.000, 0.000],                                              // point [x, y]
  [0.000, 0.940],                                              // line to here
  { "arc": { "to": [0.100, 1.040], "radius": 0.100, "dir": "cw" } }
]
```

- The first node must be a point (an arc needs a current position).
- A point node draws a straight segment from the previous position.
- An arc node draws a circular arc from the previous position to `to`, with
  the given `radius` (meters) and bend direction `dir`: `"cw"` bends
  clockwise, `"ccw"` counterclockwise, as seen in table space (+x right,
  +y up). Of the two circle centers at distance `radius` from both endpoints,
  `dir` selects one; arcs are always the **minor** arc (central angle
  ≤ 180°). This makes every arc unambiguous. For a longer bend, chain two or
  more arc nodes.
- Arc feasibility: `radius >= |to - from| / 2` (else V019 error). Degenerate
  arcs with `|to - from| < 0.001` are V019 errors.
- `closed: true` (walls only) adds a final straight segment back to the first
  node. A closed wall's interior is solid, out-of-play space — **except the
  outer boundary** (the largest-area closed wall, V003), whose interior is
  the playfield itself. That one exception is what keeps section 8.1's
  occupancy grid from marking the whole table solid.
- Paths must not self-intersect (V019 error).

```
   from o- - - - -o to        dir "cw":  the arc bulges LEFT of the
          \      /            from->to travel direction (center is on
           \    /             the right).
            \__/              dir "ccw": mirror image (bulges right,
      arc, dir "cw"           center on the left).
```

Worked example — the top-left rounded corner of the default table: current
position `[0.000, 0.940]`, node `{"arc": {"to": [0.100, 1.040], "radius":
0.100, "dir": "cw"}}`. Center is `[0.100, 0.940]`; the wall sweeps from 180°
to 90° around it, i.e. a quarter-round corner bulging up-left.

Walls collide as zero-thickness, two-sided chains of segments and arcs; open
endpoints collide as points (08-physics.md). Where an author wants a soft
rounded end (top of a lane guide), place a `post` at the endpoint — the
prefabs in section 5 do exactly that.

## 4. Element types

One subsection per canon type. Parameter tables list: name, type, unit,
required-or-default, hard valid range (violations are V019 errors; recommended
bands where given are V020 warnings). Every element also takes the common
fields of section 2. Event names are the canon 5.7 set; payload schemas are
owned by 10-scripting.md.

### 4.1 wall

Static barrier: the outer boundary, lane guides, island shapes. Physics:
08-physics.md (segment/arc CCD).

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `path` | node[] | m | required | 2–256 nodes |
| `closed` | bool | — | `false` | — |
| `material` | string | — | `"wood"` | `wood`/`rubber`/`steel`/`plastic` |

```json
{ "id": "left_guide", "type": "wall", "material": "wood",
  "path": [[0.000, 0.270], [0.135, 0.130]] }
```

Events: none. Exactly one closed wall must enclose the whole play area (the
one with the largest enclosed area is the **outer boundary**, V003). Rubbers
on real tables are walls/posts with `"material": "rubber"` (canon 5.6).

### 4.2 post

Round pin: lane separators, rubber posts, wall-end caps. Physics:
08-physics.md (circle CCD).

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | in bounds |
| `radius` | number | m | `0.008` | 0.004–0.015 |
| `material` | string | — | `"rubber"` | `wood`/`rubber`/`steel`/`plastic` |

```json
{ "id": "lane_post_1", "type": "post", "pos": [0.180, 0.930] }
```

Events: none.

### 4.3 flipper

The player-controlled bat. Physics and feel model: 08-physics.md (bespoke
constraint, canon ADR-003 — the highest-value tuning surface in the project).

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | pivot position |
| `length` | number | m | `0.076` | 0.050–0.100 |
| `radius_base` | number | m | `0.011` | 0.008–0.015 |
| `radius_tip` | number | m | `0.007` | 0.005–0.010 |
| `rest_angle_deg` | number | deg | required | [-180, 180) |
| `swing_deg` | number | deg | `52` | 30–70; positive = up-swing |
| `side` | string | — | required | `"left"`\|`"right"` |
| `input` | string | — | = `side` | `left`/`right`/`upper_left`/`upper_right` |
| `strength` | number | scale | `1.0` | 0.5–1.5 |

Semantics: `rest_angle_deg` is the direction from pivot to tip at rest.
Active angle = `rest_angle_deg + swing_deg` for `side:"left"` (CCW sweep),
`rest_angle_deg - swing_deg` for `side:"right"` (CW sweep) — both sweep the
tip **up-table**. `input` maps the flipper to a cabinet button; on two-button
cabinets `upper_left`/`upper_right` are driven by `left`/`right` (mapping in
05-engine-core.md). The shape is a capsule-like wedge from `radius_base` at
the pivot to `radius_tip` at the tip.

```json
{ "id": "upper_right", "type": "flipper", "pos": [0.410, 0.560],
  "rest_angle_deg": -149, "side": "right", "input": "upper_right" }
```

Events: none (flipper state is input-driven; scripts gate it via
`tb.set_flipper_enabled`).

### 4.4 plunger

Ball launcher. Exactly one per table (V023). The launch direction is **not**
fixed: it is the lane direction `launch_angle_deg` (default 90 = straight
up-table), so place the plunger at the bottom of the lane that angle points
along. Physics: 08-physics.md §6.16.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | ball rest center (resting face-tip position) |
| `launch_angle_deg` | number | deg | `90` | 60–120; lane direction `ĵ_lane` |
| `max_speed` | number | m/s | `7.5` | 4.0–10.0 |
| `charge_time_s` | number | s | `1.5` | 0.3–3.0 |
| `auto` | bool | — | `false` | — |
| `auto_delay_ms` | number | ms | `500` | 100–3000; only with `auto` |

`launch_angle_deg` gives the lane direction `ĵ_lane = (cos φ, sin φ)`; the
plunger face is a static segment perpendicular to it at `pos`, and the launch
impulse is added along it (08-physics.md §6.16). `auto: true` is an
autolauncher: it fires at `q = 1` (full `max_speed`) `auto_delay_ms` after a
ball settles on it, ignoring player input (used for multiball feeds). A manual
plunger releases at `v_launch = max_speed * (0.2 + 0.8 * q)` with charge
`q = min(1, held_ticks / (charge_time_s * 1000))` — a tap gives the 20 %
floor, a full 1.5 s pull gives 100 %. **08-physics.md §6.16 owns the release
curve and the skill-shot repeatability guarantee**; the 0→100 % power gauge
over 1.5 s is binding product behavior (01-product.md).

```json
{ "id": "main_plunger", "type": "plunger", "pos": [0.500, 0.030] }
```

Events: `ball_launched` when the launched ball leaves the plunger region.

### 4.5 pop_bumper

Mushroom bumper that kicks the ball away radially. Physics: 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | — |
| `radius` | number | m | `0.031` | 0.020–0.045 |
| `kick_speed` | number | m/s | `4.5` | 2.0–8.0 |
| `cooldown_ms` | number | ms | `60` | 30–200 |

```json
{ "id": "pop_main", "type": "pop_bumper", "pos": [0.240, 0.760] }
```

Events: `switch_hit` on each kick (rate-limited by `cooldown_ms`).

### 4.6 slingshot

Kicking rubber face, normally the lower triangle above each flipper. Only the
`face` segment kicks; build the triangle body from a rubber `wall` (or use
the `sling_pair` prefab). Physics: 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `face` | [[x,y],[x,y]] | m | required | length 0.040–0.100 |
| `kick_speed` | number | m/s | `3.5` | 2.0–6.0 |
| `cooldown_ms` | number | ms | `80` | 40–200 |

```json
{ "id": "left_sling_kick", "type": "slingshot",
  "face": [[0.139, 0.240], [0.165, 0.175]] }
```

Events: `switch_hit` on each kick.

### 4.7 standup_target

Fixed target face; registers a hit and rebounds the ball. Physics:
08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | face center |
| `width` | number | m | `0.025` | 0.015–0.050 |
| `facing_deg` | number | deg | required | outward face normal |
| `min_speed` | number | m/s | `0.3` | 0.1–1.0 |

`facing_deg` is the direction the face **points** (its outward normal); a
ball must arrive traveling roughly opposite. A target on the left wall facing
down-right toward the center has `facing_deg` ≈ -55.

```json
{ "id": "target_left", "type": "standup_target", "pos": [0.085, 0.620],
  "facing_deg": -55, "tags": ["lab_targets"] }
```

Events: `switch_hit` when struck at ≥ `min_speed` (slower contacts rebound
silently).

### 4.8 drop_target_bank

Row of targets that drop when hit; completing the row emits `bank_complete`.
Layer 0 only. Physics: 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `targets` | array | — | required | 2–7 of `{pos, width, facing_deg}` |
| `reset` | string | — | `"script"` | `"script"`\|`"auto"` |
| `auto_reset_ms` | number | ms | `1500` | 200–10000; only with `"auto"` |

Each entry in `targets` uses the standup semantics for `pos`, `width`
(default `0.025`), `facing_deg`. A dropped target stops colliding until the
bank resets (script calls `tb.drop_bank_reset(id)`, or automatically
`auto_reset_ms` after the last target drops when `reset:"auto"`). Targets are
indexed 1-based in array order for event payloads.

```json
{ "id": "gear_bank", "type": "drop_target_bank", "reset": "script",
  "targets": [
    { "pos": [0.150, 0.700], "facing_deg": -90 },
    { "pos": [0.183, 0.700], "facing_deg": -90 },
    { "pos": [0.216, 0.700], "facing_deg": -90 } ] }
```

Events: `target_down` per target, `bank_complete` when all are down.

### 4.9 spinner

Spinning plate in a lane; the ball drives it, it decays over time. Physics:
08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | plate center |
| `facing_deg` | number | deg | required | forward travel direction |
| `friction` | number | 1/s factor | `0.55` | 0.10–0.90 |

`facing_deg` is the ball travel direction that spins it forward; the plate
spans the lane perpendicular to it. `friction` is the per-second decay of
spin angular velocity: `omega(t + 1 s) = friction * omega(t)`.

```json
// Spinners are the ONE lane element that must NOT sit on the lane centre
// line: a level, centred plate wedges a ball that stalls on it. Offset it
// 0.0085 outboard and lean it 14 deg — 15-launch-tables.md section 0.8
// keep-out (c), which every shipped table follows.
{ "id": "orbit_spinner", "type": "spinner", "pos": [0.029, 0.700],
  "facing_deg": 76 }
```

Events: once per **full** revolution of the plate (every 2π of accumulated
plate angle) while spinning, `switch_hit{id}` then `spinner_spin{id, rpm}`,
where `rpm = |ω| * 60 / 2π` is the instantaneous plate speed at emission
(08-physics.md §6.6 owns the cadence; payload schema in 10-scripting.md
§4.1). Never per half-revolution — a 4 m/s rip is ≈ 26 event pairs.

### 4.10 gate

One-way flap across a lane. Physics: 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | gate center |
| `facing_deg` | number | deg | required | allowed pass direction |
| `width` | number | m | required | 0.020–0.080 |
| `default_state` | string | — | `"one_way"` | `one_way`/`open`/`closed` |

The flap spans `width` centered at `pos`, perpendicular to `facing_deg`.
Three states: `one_way` — a ball moving in the `facing_deg` direction
pushes through, the reverse direction is a solid wall; `open` — passable
both ways; `closed` — a wall both ways. The loader sets `default_state`.
A gate whose `default_state` is `one_way` is purely mechanical and ignores
script calls with a warn (10-scripting.md §3.4); any other gate is
controlled: `tb.gate_open(id)` sets `open`, `tb.gate_close(id)` sets
`closed`.

```json
// The plunger_lane prefab generates exactly this gate at top_y + 0.008
// (section 5.2), which is y = 0.888 with the default top_y 0.880.
{ "id": "shooter_gate", "type": "gate", "pos": [0.500, 0.888],
  "facing_deg": 90, "width": 0.040 }
```

Events: `switch_hit` when a ball passes through.

### 4.11 rollover

Wire/button switch in a lane; triggers when the ball rolls over it. Physics
sensor only (no collision): 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | wire center |
| `length` | number | m | `0.05` | 0.020–0.100 |
| `facing_deg` | number | deg | required | lane travel direction |

The sensor is a segment of `length` centered on `pos`, **aligned with**
`facing_deg` (the wire lies along the lane, the ball rolls over it
lengthwise). Triggers in either travel direction, once per crossing.

```json
{ "id": "top_lane", "type": "rollover", "pos": [0.240, 0.900],
  "facing_deg": 90 }
```

Events: `rollover`.

### 4.12 kicker

Hole/saucer that captures the ball, holds it, and ejects it. Physics:
08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | capture center |
| `radius` | number | m | `0.014` | 0.010–0.025 |
| `capture_ms` | number | ms | `800` | 100–5000 |
| `eject_speed` | number | m/s | `3.0` | 1.0–8.0 |
| `eject_angle_deg` | number | deg | saucer/scoop: required | eject velocity direction; vuk: ignored |
| `style` | string | — | required | `"saucer"`\|`"scoop"`\|`"vuk"` |

`saucer`: shallow visible dish; ball stays rendered (captures only below
3.0 m/s — fast balls fly over, 08-physics.md §6.9). `scoop`: ball vanishes
under the playfield while held. Both eject at `eject_speed` along
`eject_angle_deg`. `vuk` (vertical up-kicker): captures like a scoop but
ejects **into a ramp**: at load, the loader binds it to the ramp with a
path end within 0.03 m of the kicker `pos` — exactly one end must match
(V011). The eject puts the ball on that ramp at the matched end moving
into the path at `eject_speed`; `eject_angle_deg` is ignored (the
direction is the ramp path). The matched end becomes an internal feed with
no entry seam (08-physics.md §6.9, §6.10.2). A vuk feeding a climbing ramp
is how a kicker delivers balls to layer 1 (section 4.21). Default
behavior: eject `capture_ms` after capture; a script may hold indefinitely
with `tb.kick_hold(id)` and release with `tb.kick(id)` (10-scripting.md).
The `capture_ms` auto-eject is a sim timer that keeps running during tilt
and is never suppressed, and on `tilt` (or Duel timeout) the framework
force-ejects every captured ball — including script-held ones — at this
element's `eject_speed`/`eject_angle_deg`, so a held ball can never
deadlock the table (11-game-framework.md).

```json
{ "id": "mode_scoop", "type": "kicker", "pos": [0.120, 0.820],
  "eject_angle_deg": -60, "style": "scoop" }
```

Events: `kicker_enter` on capture.

### 4.13 ramp

Constrained 1-D path with a height profile (canon 5.6): plastic/wire ramps
that carry the ball above the playfield and between layers. Physics
(entry capture, profile-following, exit): 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `path` | node[] | m | required | 2–64 nodes; open (never closed) |
| `width` | number | m | `0.044` | 0.040–0.080; ≥ 0.044 recommended |
| `height_profile` | array | — | required | `{s, z}` keyframes, see below |
| `drop_exit` | bool | — | `false` | — |

`height_profile` keyframes: `s` is normalized arc length along `path`
(0 = first node, 1 = last), `z` in meters above layer 0. Rules (V010):
first keyframe `s: 0`, last `s: 1`, `s` strictly increasing, `z ∈ [0, 0.15]`,
and slope `|dz/ds_meters| <= 0.60` (steeper is unclimbable — 08-physics.md).
The entry keyframe `z` must equal the entry surface height (0 when entering
from layer 0). Exit: if `drop_exit` is true the ball leaves the ramp airborne
at the final z and lands ballistically (08-physics.md); if false the final z
must match the destination surface — 0 (layer 0) or `playfield.layer1_z`
±0.005 (V011).

**Seam layers are derived from the profile, never declared.** Each path end
that is not internal (`drop_exit`, vuk feed) carries an entry seam, and that
seam sits on the layer its own profile z implies (08-physics.md §6.10.2 owns
the rule, same ±0.005 tolerance as V011): `|z_end| <= 0.005` → layer 0,
`|z_end − playfield.layer1_z| <= 0.005` → layer 1, anything else → V011
error. So a ramp whose far end arrives at `layer1_z` grows a **layer-1**
seam there: a layer-1 ball binds to it and rides the ramp back down, which
is how an upper playfield drains. There is no `exit_layer` key — the exit
layer is *derived* from the final keyframe z by the same rule; writing
`exit_layer` in JSON is an unknown key (V026, ignored). The ramp element's
own `layer` field is only its **entry** layer (0 in v1, V024) and is never
used to place a seam.

```json
{ "id": "left_ramp", "type": "ramp", "width": 0.044, "drop_exit": true,
  "path": [ [0.130, 0.560], [0.130, 0.760],
            { "arc": { "to": [0.070, 0.820], "radius": 0.060, "dir": "ccw" } },
            [0.058, 0.360] ],
  "height_profile": [ { "s": 0.0, "z": 0.0 }, { "s": 0.55, "z": 0.060 },
                      { "s": 0.85, "z": 0.060 }, { "s": 1.0, "z": 0.020 } ] }
```

Events: `ramp_made` when a ball traverses from entry to exit (a ball that
rolls back down emits nothing). Wireforms are ramps with different art
(13-art-direction.md).

### 4.14 magnet

Under-playfield electromagnet, script-controlled. Physics (force law,
falloff): 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | core center |
| `strength` | number | N at 0.05 m | `1.2` | 0.2–5.0 |
| `radius` | number | m | `0.09` | 0.030–0.200 |
| `default_on` | bool | — | `false` | — |

`strength` is the attracting force on the ball at 0.05 m from `pos`;
`radius` is the effect cutoff. Controlled with `tb.magnet_on/off/pulse`.

```json
{ "id": "drift_magnet", "type": "magnet", "pos": [0.100, 0.480],
  "strength": 1.6 }
```

Events: none (magnets are actuators, not switches).

### 4.15 captive_ball

A ball permanently trapped in a short slot; the free ball strikes it.
Physics: 08-physics.md (ball–ball transfer along the slot).

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `slot` | `{a:[x,y], b:[x,y]}` | m | required | length 0.040–0.120 |

`a` is the near end where the captive ball rests (the striking end faces the
open lane); `b` is the far end. The captive ball has standard radius 0.0135.

```json
{ "id": "milkshake", "type": "captive_ball",
  "slot": { "a": [0.300, 0.640], "b": [0.300, 0.710] } }
```

Events: **two, and there is no flag on either**.

1. `switch_hit{id, ball_id, speed, tags}` — the standard switch payload
   (10-scripting.md §4.1), emitted on the **free ball's impact** with the
   captive ball. `speed` is the impact speed of the striking ball in m/s
   (not the captive's). `ball_id` is the striking ball.
2. `captive_full_travel{id}` — the specialized event (canon 5.7), emitted
   when the **captive ball reaches the far end `b` with speed ≥ 0.3 m/s**
   (08-physics.md §6.13; the captive bounces there with `e = 0.4`). A weak
   nudge that never carries the captive to `b`, and a hit on the near end
   `a`, emit nothing extra.

A solid full-travel hit therefore produces `switch_hit` on impact and
`captive_full_travel` a few dozen ticks later, when the captive actually
arrives — they are separate events at separate times, so scripts award the
"rattle" and the "full travel" independently. Payload schemas are owned by
10-scripting.md §4.1.

### 4.16 ball_lock

Holds balls for multiball. Layer 0 only. Physics: 08-physics.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | lock mouth center |
| `capacity` | integer | balls | required | 1–3 |
| `style` | string | — | `"visible"` | snake_case art hint, see below |
| `eject_speed` | number | m/s | `2.5` | 1.0–8.0 |
| `eject_angle_deg` | number | deg | `-90` | release eject direction |

A ball entering the mouth is captured and removed from play. `style` is a
free-form snake_case art hint (visual treatment in 13-art-direction.md):
the reserved value `"hidden"` hides held balls; any other value keeps them
rendered at the lock (`"visible"` default; shipped tables use themed hints
like `"magnet"`, `"bay"`, `"tent"`, `"vault"`). Scripts release with
`tb.release_lock(id, n)`; released balls re-enter at `pos`, **one ball every
500 ms** (the same cadence in 08-physics.md §6.14 and 10-scripting.md §3.4 —
one number, three docs), ejected at `eject_speed` along `eject_angle_deg`
(default −90°: straight down-lane toward the player), with push-out after
each. **08-physics.md §6.14 owns the eject kinematics and the release
cadence**; this table declares the two parameters, and any question about
how the ball leaves defers there. Locked-ball accounting interacts with the
trough and `tb.add_ball` (11-game-framework.md).

Capture is unconditional and belongs to the sim: a free ball entering the
mouth while `held < capacity` is captured, and there is no script confirm
step. A script that does not want the ball calls `tb.release_lock(id, 1)`
in its `ball_lock` handler (the mandatory unlit-lock pattern,
10-scripting.md). If a locked ball is neither released nor claimed within
3000 ms the sim auto-releases one ball and logs a warning, mirroring the
kicker `capture_ms` failsafe. On `tilt` (and on Duel timeout) every held
ball is force-ejected at `eject_speed`/`eject_angle_deg`
(11-game-framework.md).

```json
{ "id": "crane_lock", "type": "ball_lock", "pos": [0.420, 0.900],
  "capacity": 3 }
```

Events: `ball_lock` on each capture.

### 4.17 outhole

Drain sensor. At least one per table (V008). Layer 0 only.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `region` | `{a:[x,y], b:[x,y]}` | m | required | length 0.030–0.500 |

Any ball whose center crosses the segment `a`–`b` is captured and sent to the
trough. Place it below the flippers, spanning the drain funnel — and **not**
crossing the shooter lane, or it will eat the ball waiting on the plunger.

```json
{ "id": "drain", "type": "outhole",
  "region": { "a": [0.020, 0.010], "b": [0.460, 0.010] } }
```

Events: `drain` (ball-save logic reacts to this; 10-scripting.md,
11-game-framework.md).

### 4.18 trough

The off-playfield ball store feeding the plunger. Exactly one per table
(V023). Logical element: no position, no collision.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `capacity` | integer | balls | `4` | 1–6 |

`capacity >= playfield.ball_count` (V009). Feed timing and multiball
mechanics: 08-physics.md and 11-game-framework.md.

```json
{ "id": "main_trough", "type": "trough", "capacity": 4 }
```

Events: none directly (drain/launch events come from outhole and plunger).

### 4.19 light

Playfield insert light. No collision; rendered by 13-art-direction.md;
controlled by `tb.light_on/off/blink`.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | — |
| `shape` | string | — | `"circle"` | `"circle"`\|`"arrow"`\|`"ring"` |
| `size` | number | m | `0.012` | 0.004–0.050 |
| `color` | string | — | required | palette role or `"#rrggbb"` |
| `direction_deg` | number | deg | `90` | arrows only; points this way |
| `function` | string | — | optional | 13-art-direction.md §6 tag, see below |

`color` is either a palette role name resolved through `art.json`
(13-art-direction.md) or a literal hex color. An unresolvable role is V027
(warning; renders white). `function` optionally tags the insert's gameplay
role for the cross-table color conventions of 13-art-direction.md §6: one
of `shot_arrow`, `jackpot`, `lock`, `bonus`, `ball_save`, `extra_ball`,
`special`, `mode`, `progression`, `multiball_ready`, `status`. An unknown
tag is V031 (warning); 13-art-direction.md's checks additionally warn when
`color` contradicts the tag's canonical color.

```json
{ "id": "light_top_lane", "type": "light", "pos": [0.240, 0.860],
  "shape": "arrow", "direction_deg": 90, "color": "#00e5ff" }
```

Events: none. Lights start off unless the script turns them on.

### 4.20 toy

Decorative mechanical piece (spinning sign, crane, figure) with optional
collision. Art and animation hooks: 13-art-direction.md.

| Param | Type | Unit | Req/default | Range |
|---|---|---|---|---|
| `pos` | [x,y] | m | required | anchor |
| `collider` | node[] | m | optional | open or closed path |
| `art_ref` | string | — | required | art group name in art.json (V002) |
| `anim` | string[] | — | `[]` | named animation hooks |

```json
{ "id": "robot_head", "type": "toy", "pos": [0.260, 0.880],
  "art_ref": "robot_head_group", "anim": ["nod", "spark"],
  "collider": [[0.230, 0.860], [0.290, 0.860]] }
```

Events: `switch_hit` on collider contact (only if `collider` present).

### 4.21 Layers

`layer` 0 is the main playfield; `layer` 1 is an upper playfield at height
`playfield.layer1_z`. A layer-1 ball runs the same 2-D physics at fixed z
(08-physics.md). Balls move between layers **only** via ramps: climbing to
`layer1_z` going up (shot from the playfield, or fed by a vuk kicker's
eject — section 4.12), and via ramps or `drop_exit` ramp ends coming down
(08-physics.md §6.11). Layer-1 regions must be fully enclosed by layer-1
walls except where a ramp end meets them (V011).

Allowed on layer 1: `wall`, `post`, `flipper`, `pop_bumper`, `slingshot`,
`standup_target`, `spinner`, `gate`, `rollover`, `kicker`, `magnet`,
`captive_ball`, `light`, `toy`. Layer 0 only (V024): `plunger`,
`drop_target_bank`, `ball_lock`, `outhole`, `trough`, `ramp` (a ramp's
`layer` is its entry layer, always 0 in v1 — it may still *end* on layer 1,
and that end carries a **layer-1 seam** derived from its profile z, so a
layer-1 ball binds to it and rides back down: section 4.13).

## 5. Prefab generators

Prefabs are macro elements that expand into primitives at load time
(canon 5.6). They are the quality lever for LLM authoring: each encodes
geometry that is hard to get right node-by-node. Prefab instances live in the
top-level `prefabs` array:

```json
{ "id": "flippers", "prefab": "flipper_pair_standard",
  "pos": [0.240, 0.115], "tags": ["lower"] }
```

Binding expansion rules:

- Expansion happens in the loader **before validation**; validators and
  scripts see only the expanded primitive elements. `tb_validate` reports
  errors on expanded elements against the prefab instance's JSON pointer.
- Generated ids are `<instance_id>_<suffix>` with the exact suffixes listed
  per prefab below. Scripts must use these ids (e.g. `flippers_left_flipper`).
- Children inherit the instance's `layer` and `tags`.
- Prefabs expand in array order; children in the documented order. Expansion
  is pure and deterministic (same input ⇒ same elements, always).
- Hand-placed primitives coexist freely with prefabs; a table may use all
  prefabs, none, or a mix. Prefabs impose no layout beyond their own
  children. To customize beyond the parameters, copy the documented expansion
  into `elements` as primitives and delete the prefab instance — never expect
  edits to generated elements to persist.
- An id collision between a generated id and any other id is V001.

All defaults below are tuned to the default 0.52 × 1.04 playfield with a
0.040 shooter lane on the right (playable width 0.480, playable center
x = 0.240). On other sizes, override positions.

### 5.1 flipper_pair_standard

The standard lower flipper pair. Gets the single most important number on the
table right: the **tip gap**.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | midpoint between the two pivots |
| `tip_gap` | number m | `0.068` | 0.054–0.090 (V005) |
| `length` | number m | `0.076` | 0.050–0.100 |
| `rest_slope_deg` | number deg | `31` | 20–40; droop below horizontal |
| `swing_deg` | number deg | `52` | 30–70 |
| `strength` | number | `1.0` | 0.5–1.5 |

Tip gap = distance between the two tip **centers** at rest. The canonical
0.068 m is ≈ 2.5 ball diameters (ball = 0.027 m): wide enough that a centered
ball can drain (the game needs that threat), narrow enough that drains feel
earned, and it supports post passes and flipper-to-flipper transfer shots.
Below 0.054 center drains vanish and the table is toothless; above 0.090 the
drain rate is punishing.

Geometry: pivot separation `D = tip_gap + 2 * length * cos(rest_slope_deg)`
(default: `0.068 + 2*0.076*cos(31°) = 0.198`).

Expansion (2 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_left_flipper` | flipper | `pos = [pos.x - D/2, pos.y]`, `rest_angle_deg = -rest_slope_deg`, `side:"left"`, `input:"left"` |
| `<id>_right_flipper` | flipper | `pos = [pos.x + D/2, pos.y]`, `rest_angle_deg = rest_slope_deg - 180`, `side:"right"`, `input:"right"` |

```
   pivot o----____                      ____----o pivot
   [x-D/2]        ----___        ___----        [x+D/2]
                         tip   tip
                          |<--->|  tip_gap 0.068
```

### 5.2 plunger_lane

Right-side shooter lane: inner wall, rounded top post, one-way exit gate,
plunger. The post exists only in the standalone form; the *merged variant*
below omits it.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `lane_width` | number m | `0.040` | 0.033–0.050 |
| `top_y` | number m | `0.880` | 0.60–0.95 |
| `auto` | bool | `false` | — |
| `max_speed` | number m/s | `7.5` | 4.0–10.0 |
| `charge_time_s` | number s | `1.5` | 0.3–3.0 |

Let `W = playfield.size[0]`, `xw = W - lane_width` (inner wall x), and
`y_gate = top_y + 0.008` (the gate line, and the top of the inner wall).

Expansion (4 elements — **3 in the merged variant**, which emits no
`<id>_top_post`):

| Child id | Type | Values |
|---|---|---|
| `<id>_wall` | wall | `path [[xw, 0.000], [xw, y_gate]]`, wood |
| `<id>_top_post` | post | `pos [xw - 0.008, y_gate]`, radius 0.008, rubber — **standalone form only** |
| `<id>_gate` | gate | `pos [W - lane_width/2, y_gate]`, `width lane_width`, `facing_deg 90` |
| `<id>_plunger` | plunger | `pos [W - lane_width/2, 0.030]`, `max_speed`, `charge_time_s`, `auto` |

```
        |p|g|   g = one-way gate on the gate line y = top_y + 0.008
        | | |       (ball exits up, cannot fall back in)
  play  |w| |   p = <id>_top_post (standalone form only), tangent to the wall
  field |a| |       on the PLAYFIELD side, so it takes nothing out of the lane
        |l|o|   w = <id>_wall at x = W - lane_width, running from y = 0
        |l|=|       to the gate line; o = ball on <id>_plunger (=)
```

The wall starts at y = 0 so the lane is watertight against the bottom wall,
and ends **on** the gate line so the gate's inner endpoint lands on a
collider. The ball exits at the top moving +y and curves left along the
outer top arc into the playfield.

Two numbers this prefab exists to get right, on the default table
(`W = 0.520`, `lane_width = 0.040`, `top_y = 0.880` ⇒ `xw = 0.480`,
`y_gate = 0.888`):

- **V014 gate-span clearance.** The flap is perpendicular to
  `facing_deg 90`, so its endpoints are `[xw, y_gate] = [0.480, 0.888]` and
  `[W, y_gate] = [0.520, 0.888]`. The inner endpoint sits exactly on
  `<id>_wall`'s top node ⇒ distance **0.000 m** (in the standalone form
  `<id>_top_post`'s surface is tangent there too, but the wall node is what
  carries the measurement, so the merged variant measures the same);
  the outer endpoint sits on the outer boundary wall's right leg ⇒ distance
  **0.000 m**. Both are ≤ V014's 0.006 m tolerance, so the prefab is
  warning-free out of the box. The gate line and the wall top are the same
  line, `top_y + 0.008`, and must stay so: lifting the gate to `top_y + 0.015`
  leaves 0.015 m to the wall and 0.007 m to the post surface — two V014
  warnings on every table that uses the prefab.
- **Lane clearance past the post.** `<id>_top_post` is centered at
  `xw - 0.008`, i.e. tangent to the wall line from the playfield side: its
  rightmost point is exactly `[xw, y_gate]`, so the lane-side surface runs
  wall→post with a continuous tangent and the post takes **0 mm** out of
  the lane. Lane clearance stays `lane_width` (0.040 m default) end to end,
  above the 0.033 minimum and clear of the 0.025–0.032 jam band (section 6)
  for every `lane_width` in range. The post rounds the wall's open end, which
  is its whole job (section 3) — and in the merged variant the wall's own end
  cap does that job instead.

**Merged variant: no top post.** The lane is **merged** when the table also
carries an `orbit` (section 5.5) whose right mouth node
`[W - mouth_x, entry_y_right]` lies within **0.045 m** of this lane's wall top
node `[xw, y_gate]` — the condition under which the two lanes are one channel
(defaults: `sqrt(0.035^2 + 0.012^2)` = 0.037). A merged instance emits **three**
elements and no `<id>_top_post`; every other instance (test-lab, or any table
with no orbit) emits four and keeps it. Reader and validator decide the same
way: measure those two nodes.

The post must go because at a merge junction it stands *in the mouth*. With the
launch-table numbers its centre `[0.472, 0.888]` is **0.002595 m** from the
mouth line `[0.480, 0.888]`-`[0.445, 0.900]`, less than its own 0.008 radius,
so the disc straddles the aperture; the widest surface gap it leaves is
`sqrt(0.027^2 + 0.012^2) - 0.008` = **0.021547 m**, under the 0.027 ball, and
the mouth stops passing in either direction. And with the merge flap closed
(15-launch-tables.md §0.8) post and flap form an upward-opening V that parks a
ball at `[0.486145, 0.904192]`: the contact normals 48.86° (post) and 108°
(flap) bracket the 90° that has to carry gravity, so it rests there forever —
and `tb_validate`, which measures gaps and not contact cones, cannot see it.

Dropping it costs nothing, because `<id>_wall` already does the post's job: a
wall segment collides as a capsule and its end is a rounded cap
(08-physics.md's swept-circle-vs-segment resolves the endpoint itself), so the
lane wall's open end is round whether or not a post sits on it. The gate's
inner endpoint still measures **0.000 m** — that measurement was always against
`<id>_wall`'s top node, which the post merely coincided with.

Keep `y_gate` below the outer wall's top corner arc (y = 0.940 on the
default table) or the gate's outer endpoint stops landing on a collider and
V014 warns.

### 5.3 sling_pair

Two slingshot assemblies (rubber triangle wall + active kicking face) above
the flippers.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | center point between the slings |
| `spread` | number m | `0.150` | 0.120–0.200; bottom-corner separation |
| `face_length` | number m | `0.070` | 0.050–0.090 |
| `tilt_deg` | number deg | `22` | 10–35; face lean from vertical |
| `kick_speed` | number m/s | `3.5` | 2.0–6.0 |

Left-side geometry (right side mirrors about `pos.x`):

```
B = [pos.x - spread/2, pos.y - 0.035]                 // bottom-inner corner
T = B + face_length * [-sin(tilt_deg), cos(tilt_deg)] // top corner
K = B + [-0.038, 0.015]                               // back corner
```

Expansion (4 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_left_wall` | wall | closed path `[B, T, K]`, rubber |
| `<id>_left_sling` | slingshot | `face [T, B]`, `kick_speed` |
| `<id>_right_wall` | wall | mirrored triangle, rubber |
| `<id>_right_sling` | slingshot | mirrored face |

```
        T\           /T          face T-B is the active kicker,
        | \         / |          pointing in toward the flippers.
        K  \       /  K          K rounds out the back.
         \  \     /  /
          \__\B  B/__/
       flipper   flipper
```

The bottom corners `B` sit ≈ 0.02–0.035 m above and slightly outboard of the
flipper pivots so inlane balls feed the flipper, not the sling face.

### 5.4 inlane_outlane_pair

One side's inlane/outlane assembly: angled lower side wall, lane divider with
rubber top post, two rollovers, two lights. Instantiate once per side.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `side` | string | required | `"left"`\|`"right"` |
| `mirror_axis_x` | number m | `0.240` | playable center; used for `"right"` |
| `wall_top` | [x,y] | `[0.000, 0.360]` | on the outer wall |
| `wall_bottom` | [x,y] | `[0.096, 0.058]` | above the drain area |
| `divider_top` | [x,y] | `[0.062, 0.268]` | — |
| `divider_bottom` | [x,y] | `[0.122, 0.138]` | near flipper pivot |

Coordinates are given for `side:"left"`; for `"right"` every x maps to
`2 * mirror_axis_x - x` and lane-direction angles mirror (`a → 180 - a`).

Expansion (7 elements):

| Child id | Type | Values (left side) |
|---|---|---|
| `<id>_side_wall` | wall | `path [wall_top, wall_bottom]`, wood |
| `<id>_divider` | wall | `path [divider_top, divider_bottom]`, wood |
| `<id>_top_post` | post | `pos divider_top`, radius 0.008, rubber |
| `<id>_outlane_rollover` | rollover | `pos [0.045, 0.240]`, `facing_deg -72` |
| `<id>_inlane_rollover` | rollover | `pos [0.105, 0.185]`, `facing_deg -65` |
| `<id>_outlane_light` | light | `pos [0.052, 0.205]`, circle |
| `<id>_inlane_light` | light | `pos [0.115, 0.155]`, circle |

Lights use color role `"insert_primary"` (13-art-direction.md).

```
  outer |  \ o  \ i          o = outlane (drains), i = inlane
  wall  |   \    \  sling      (feeds flipper). Divider has a
        |    \  post           rubber post on top: a live catch
        |     \ | \_flipper    surface.
```

The outlane channel (side wall to divider) is ≈ 0.038 m wide at the top —
above the 0.033 minimum, tight enough to feel dangerous. These defaults mesh
with `flipper_pair_standard` at `[0.240, 0.115]` and `sling_pair` at
`[0.240, 0.210]`; if you move those, run `tb_validate` and adjust until the
clearance checks (V006) pass.

### 5.5 orbit

Full left-to-right loop around the top of the table. **An orbit hugs the
outer wall**: the table's own boundary *is* the lane's outer edge, and the
prefab emits only what is inboard of the ball — one guide wall and the two
entry switches. Nothing is ever generated outside the lane, so no strip can
exist between the orbit and the boundary.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `mouth_x` | number m | `0.075` | 0.038–0.100; x of the lane's **inner** edge — the guide wall — at the mouth, mirrored to `W - mouth_x` on the right. The outer edge is the boundary, so `mouth_x` **is** the lane width |
| `top_radius` | number m | `0.130` | 0.100–0.160; must match the table's own outer-wall corner radius |
| `entry_y_left` | number m | `0.550` | 0.40–0.80 |
| `entry_y_right` | number m | `0.900` | `>= plunger_lane.top_y + 0.012` |

There is no `lane_width` parameter: an orbit that hugs the boundary has
exactly one free dimension, and `mouth_x` is it.

Let `W`, `H` = `playfield.size` and `R = top_radius`. The table's outer wall
has its top corner arcs centered at `[R, H - R]` and `[W - R, H - R]`
(`[0.130, 0.910]` and `[0.390, 0.910]` on the default table with the default
`R`). The guide wall is that boundary **inset by `mouth_x`**:

```
guide_path(d = mouth_x) =
  [[d, entry_y_left], [d, H - R],
   arc cw r (R - d) to [R, H - d],
   [W - R, H - d],
   arc cw r (R - d) to [W - d, H - R],
   [W - d, entry_y_right]]
```

The three lines an author needs (`W = 0.520`, `H = 1.040`, defaults):

```
lane outer edge = the boundary   x = 0.000 / 0.520, top run y = 1.040
guide (inner) wall               x = 0.075 / 0.445, top run y = 0.965,
                                 corner arcs r 0.055 about [0.130, 0.910]
                                 and [0.390, 0.910] (the boundary's centers)
lane center                      x = 0.0375 / 0.4825, top run y = 1.0025
```

Worked values on the default table (`W = 0.520`):

| `mouth_x` | Left lane | Left center | Right lane | Right center |
|---|---|---|---|---|
| `0.045` | [0.000, 0.045] | **0.0225** | [0.475, 0.520] | **0.4975** |
| `0.075` (default, and every launch table) | [0.000, 0.075] | **0.0375** | [0.445, 0.520] | **0.4825** |

Every element that lives in the orbit lane — light, magnet — sits on that
center line, and the mouths (the guide wall's two lower nodes) are at
x 0.075 / 0.445 as 15-launch-tables.md declares them.

**Spinners are the exception, and it is a stuck-ball exception.** A level
plate centred in the lane leaves `0.0375 - 0.0125` = 0.025 m to each wall,
under the 0.027 m a ball needs, so both end caps pocket a ball that stalls
on the plate — and 08-physics.md §6.6 turns the plate into a steel wall
below 0.15 m/s of pass speed, so the ball never restarts. 15's §0.8
keep-out (c) therefore places orbit spinners **0.0085 m outboard of the
lane line** (x 0.029 / 0.491 at the default `mouth_x`) with `facing_deg`
**76 / 104** — a 14° lean, past the 12.675° at which gravity beats rolling
resistance — leaving a 0.03387 m passable gap on the inboard side as the
escape route. Every shipped table follows it; see 15 §0.8 for the derived
0.31 m/s crossing floor and the per-table crossing-speed table.

A `spinner`'s plate does **not** scale with the lane —
08-physics.md §6.6 fixes its trigger segment at **0.025 m** centred on `pos`
— and at the keep-out (c) placement it covers the lane as follows. The
14°-leaned plate spans `0.0125 * cos 14°` = **0.01213 m** either side of
`pos` horizontally, so with a ball radius at each end it trips any ball
whose centre lies within **0.02563 m** of `pos`: at x 0.029 that is the
centre band **0.00337 – 0.05463 m**. A ball in a 0.075 m lane occupies
centres 0.0135 – 0.0615, so the plate catches every ball except one
hugging the guide wall within the outermost **0.00687 m** of centre travel
— which the lean then feeds back inboard rather than trapping. Do not
widen an orbit past ~0.079 m expecting a full-lane switch: past that the
uncovered band grows on both sides and the spinner stops being reliable.

Feasibility (V019 on the expanded wall):

- `mouth_x <= top_radius - 0.010`, so the guide's corner arcs keep a radius
  of at least 0.010 m (defaults: `0.130 - 0.075 = 0.055`).
- On a table whose plunger feeds the orbit,
  `mouth_x >= plunger_lane.lane_width + 0.033`, so the right mouth clears the
  shooter wall by at least a minimum lane width (defaults:
  `0.040 + 0.033 = 0.073 <= 0.075`). This is the rule section 6's orbit-lane
  row defers to: 0.045 is the floor, and a merged orbit's extra width is the
  shooter lane's own offset, not slop.

Expansion (3 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_guide_wall` | wall | `guide_path(mouth_x)`, wood |
| `<id>_left_switch` | gate | `pos [mouth_x/2, entry_y_left + 0.05]`, `width mouth_x`, `facing_deg 90`, `default_state "open"` |
| `<id>_right_switch` | gate | `pos [W - mouth_x/2, entry_y_right + 0.02]`, `width mouth_x`, `facing_deg 90`, `default_state "open"` |

The entry switches are `open` gates, not rollovers, because a rollover wire
lies *along* the lane and senses only within a ball radius (0.0135 m) of its
line — 0.027 m of a 0.075 m lane, missing every ball that rides a wall. An
`open` gate spans the full lane width, never collides, and emits
`switch_hit` on each pass in either direction. They are purely sensors:
nothing scripts them open or closed. With the defaults they land at
`[0.0375, 0.600]` and `[0.4825, 0.920]`, spanning 0.075 m; at y = 0.920 the
real lane is 0.0755 m wide (guide arc x = 0.4441, boundary arc x = 0.5196),
so both endpoints sit within 0.001 m of a collider and V014 is quiet.

```
    ___________
   /   _____   \      the outer edge is the table boundary itself; the
  |  //     \\  |     prefab draws only <id>_guide_wall, mouth_x inboard
  |  ||     ||  |     of it all the way around the top.
  |  ||     ||  |
  |x |      | x|      x = entry switches, spanning the lane
```

**Merging with the plunger.** Two prefab instances meet here, so name them
apart: `<lane>` is the `plunger_lane` instance (section 5.2), `<orbit>` is this
one — this prefab has no `_wall` child at all. With a default `plunger_lane`
the orbit's right leg *is* the shooter lane continued: `<lane>_wall` stops at
its own gate line `y_gate = 0.888` (section 5.2) and `<orbit>_guide_wall`
starts at `[0.445, 0.900]`, so above that line one
channel runs from the plunger, over the top and down the left leg — the
classic layout, in which a plunged ball rides the orbit with nothing
hand-placed to make it happen. The right mouth is the aperture between
`<orbit>_guide_wall`'s lower right node `[0.445, 0.900]` and `<lane>_wall`'s
top node `[0.480, 0.888]`: `sqrt(0.035^2 + 0.012^2) = 0.037 m`, passable and
clear of section 6's 0.025–0.032 jam band. Nothing stands in it: this is
exactly the merged case in which `<lane>` emits **no** `<lane>_top_post`
(section 5.2), whose 0.008 m disc would otherwise straddle the mouth line and
cut the widest passage to 0.021547 m. Balls coming *down* the right leg must not
be able to stop on the shooter lane's closed gate; the one deflector that
guarantees it is 15-launch-tables.md §0.8's merge flap.

### 5.6 ramp_standard

Entry funnel + climbing ramp with a computed height profile.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `entry` | [x,y] | required | on layer 0 |
| `entry_dir_deg` | number deg | `90` | climb direction at entry |
| `exit` | [x,y] | required | landing point |
| `climb_length` | number m | `0.200` | 0.120–0.350 |
| `apex_z` | number m | `0.060` | 0.040–0.120 |
| `exit_z` | number m | `0.020` | 0.000–0.090 |
| `drop_exit` | bool | `true` | — |
| `width` | number m | `0.044` | 0.040–0.080 |

Path generation: `P1 = entry + climb_length * dir(entry_dir_deg)`; then one
arc to `exit` with radius `r = clamp(|P1 - exit| / 2, 0.050, 0.200)` bending
`cw` if `exit` lies to the right of the climb direction, else `ccw`. Height
profile: `[{s:0, z:0}, {s:0.55, z:apex_z}, {s:0.85, z:apex_z},
{s:1, z:exit_z}]`. For a ramp that lands on layer 1, set `drop_exit:false`
and `exit_z = playfield.layer1_z`.

Expansion (3 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_ramp` | ramp | generated path + profile, `width`, `drop_exit` |
| `<id>_left_guide` | wall | entry funnel: from `entry + perp * (width/2 + 0.004)` extending 0.060 against `entry_dir_deg`, wood |
| `<id>_right_guide` | wall | mirror of left guide |

`perp = dir(entry_dir_deg + 90)`. The guides make a short funnel so
near-miss shots feed the ramp mouth instead of clipping its edge. Events:
the child ramp emits `ramp_made` under the id `<id>_ramp`.

### 5.7 inner_loop

Short curved lane behind the mid-table (an incomplete annulus); a ball can
loop through and come back out.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | circle center |
| `radius` | number m | `0.075` | 0.050–0.120; centerline |
| `lane_width` | number m | `0.040` | 0.033–0.055 |
| `opening_deg` | number deg | `120` | 60–160; span of the mouth |
| `rotation_deg` | number deg | `-90` | direction the mouth faces |

Both walls span `360 - opening_deg` degrees centered opposite the mouth; the
generator emits each wall as **two chained arcs** (arcs are minor-only,
section 3). Outer wall radius `radius + lane_width/2`, inner wall radius
`radius - lane_width/2`.

Expansion (3 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_outer_wall` | wall | two-arc path, wood |
| `<id>_inner_wall` | wall | two-arc path, wood |
| `<id>_switch` | rollover | at the centerline point opposite the mouth, `facing_deg` = tangent direction there |

```
        ___
       / _ \        default: mouth faces down (-90); switch x at
      | |x| |       the apex; ball enters one side, exits the
      | | | |       other.
      ^^   ^^  mouth
```

### 5.8 horseshoe

U-shaped lane: in one leg, 180° around, out the other.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | center of the bend circle |
| `radius` | number m | `0.060` | 0.045–0.100; centerline |
| `lane_width` | number m | `0.042` | 0.033–0.055 |
| `leg_length` | number m | `0.110` | 0.050–0.250 |
| `opening_dir_deg` | number deg | `-90` | legs extend this way |

Legs are parallel, `2 * radius` apart center-to-center, extending
`leg_length` from the bend in the `opening_dir_deg` direction. The 180° bend
is emitted as two 90° arcs per wall.

Expansion (4 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_outer_wall` | wall | outer U (radius `radius + lane_width/2`), wood |
| `<id>_inner_wall` | wall | inner U (radius `radius - lane_width/2`), wood |
| `<id>_left_switch` | rollover | mid-leg, left leg, facing along the leg |
| `<id>_right_switch` | rollover | mid-leg, right leg, facing along the leg |

```
       ____
      / __ \       bend at pos (two 90-degree arcs per wall)
     | |  | |
     |x|  |x|      x = rollovers, one per leg
     | |  | |
```

### 5.9 pop_cluster

Three pop bumpers in an equilateral triangle — the classic nest.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | centroid |
| `spacing` | number m | `0.085` | 0.070–0.120; center-to-center |
| `rotation_deg` | number deg | `0` | — |
| `kick_speed` | number m/s | `4.5` | 2.0–8.0 |

Vertex distance from centroid `d = spacing / sqrt(3)` (default 0.049).
Vertices at angles `90 + rotation_deg`, `210 + rotation_deg`,
`330 + rotation_deg` from the centroid.

Expansion (3 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_pop_1` | pop_bumper | top vertex (angle 90 + rotation) |
| `<id>_pop_2` | pop_bumper | lower-left vertex (210 + rotation) |
| `<id>_pop_3` | pop_bumper | lower-right vertex (330 + rotation) |

```
        (1)          spacing 0.085 center-to-center leaves a
       /   \         0.023 surface gap between bumper caps
     (2)---(3)       (radius 0.031): the ball rattles, never
                     wedges. Below 0.070 spacing the ball jams.
```

Surface gap between caps = `spacing - 2 * 0.031`; keep it below 0.027 (ball
diameter) so the ball cannot pass through the middle, or ≥ 0.033 if you want
a passable nest — never in between (jam band, section 6).

### 5.10 drop_bank_n

A row of `count` drop targets as one bank, with an insert light per target.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | bank center |
| `count` | integer | `3` | 2–7 |
| `facing_deg` | number deg | `-90` | outward normal of the row |
| `target_width` | number m | `0.025` | 0.015–0.050 |
| `gap` | number m | `0.008` | 0.004–0.020 |
| `reset` | string | `"script"` | `"script"`\|`"auto"` |
| `auto_reset_ms` | number ms | `1500` | 200–10000 |
| `lights` | bool | `true` | — |

Targets sit on the line through `pos` perpendicular to `facing_deg`
(direction `along = dir(facing_deg + 90)`). Target k (1-based) center:
`pos + (k - (count+1)/2) * (target_width + gap) * along`. Lights sit
`0.030 * dir(facing_deg)` in front of each target.

Expansion (1 + count elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_bank` | drop_target_bank | `targets` in index order, `reset`, `auto_reset_ms` |
| `<id>_light_1` … `<id>_light_<count>` | light | circle, in front of targets 1…count (omitted when `lights:false`) |

```
   [1] [2] [3]      facing_deg -90: faces the player; targets
    *   *   *       numbered along +x. * = <id>_light_k
```

### 5.11 top_lanes_n

`count` parallel rollover lanes with divider walls and lights — the classic
top lanes fed by the orbit/plunger.

| Param | Type | Req/default | Range |
|---|---|---|---|
| `pos` | [x,y] | required | center of the lane row |
| `count` | integer | `3` | 2–5 |
| `lane_width` | number m | `0.040` | 0.033–0.050 |
| `lane_length` | number m | `0.070` | 0.050–0.120 |
| `rotation_deg` | number deg | `0` | 0 = lanes run up-down |

Dividers: `count + 1` walls of length `lane_length`, centered at `pos.y`,
wall i (1-based) at x offset `(i - (count+2)/2) * lane_width` from `pos.x`
(then rotated by `rotation_deg` about `pos`). Each divider gets a rubber post
cap at its **top** end. Lane j center offset: `(j - (count+1)/2) *
lane_width`. Rollovers at lane centers; lights `0.055` below lane centers
(down-lane).

Expansion (4·count + 2 elements):

| Child id | Type | Values |
|---|---|---|
| `<id>_wall_1` … `<id>_wall_<count+1>` | wall | vertical dividers, wood |
| `<id>_post_1` … `<id>_post_<count+1>` | post | top-end caps, radius 0.008, rubber |
| `<id>_lane_1` … `<id>_lane_<count>` | rollover | lane centers, `facing_deg 90 + rotation_deg` |
| `<id>_lane_light_1` … `<id>_lane_light_<count>` | light | below each lane |

```
   o| |o| |o| |o     o = rubber post caps (live surfaces the
    | | | | | |          ball bounces between)
    |x| |x| |x|      x = rollovers, lanes numbered 1..n from
    *   *   *            the -offset side; * = lane lights
```

Number lanes 1…count from the negative-offset side (left when
`rotation_deg` is 0).

## 6. Distances an author must know

Quick reference; all values meters. The validator enforces the starred rows.

| Quantity | Value | Why |
|---|---|---|
| Ball diameter | 0.027 | Everything below derives from it (radius 0.0135, canon 5.3) |
| Minimum lane width * | 0.033 | Ball + 6 mm; below this the ball scrapes or wedges (V006) |
| Comfortable lane width | 0.040 | Ball + 13 mm; smooth flow, use for shooter/top lanes |
| Flipper length | 0.076 | Default bat; two of these plus the tip gap set the lower-table width |
| Flipper tip gap * | 0.068 | Center-to-center at rest; ≈ 2.5 ball diameters; drains possible but earned (V005) |
| Minimum ramp width | 0.044 | Ball + 17 mm; a climbing ball wanders sideways and needs the margin |
| Outlane width | 0.034–0.042 | Narrower plays unfair (instant drains); wider defangs the outlane |
| Post surface gap: rejects ball | ≤ 0.024 | Safely blocks; ball bounces off cleanly |
| Post surface gap: jam band | 0.025–0.032 | Never use: ball wedges or rattles stuck (surface gap = center dist − 2·radius) |
| Post surface gap: passes ball | ≥ 0.033 | Clean passage, same as minimum lane width |
| Slingshot face length | 0.050–0.090 | Shorter misses most balls; longer dominates the lower table |
| Sling bottom corner to flipper pivot | 0.020–0.035 | Sling sits just above/outboard of the pivot so inlanes feed the flipper |
| Orbit lane width | ≥ 0.045; **0.075** merged | 0.045 is the floor — below it a fast orbit rattles and dies. A boundary-hugging orbit sets it with `mouth_x`, and one that merges with a `plunger_lane` must also clear the shooter wall: `mouth_x ≥ lane_width + 0.033` (section 5.5), i.e. 0.075 on every launch table |
| Orbit top corner radius | 0.100–0.160 | Smaller kills orbit speed; larger eats the top of the table |
| Pop bumper spacing (cluster) | 0.085 | Center-to-center; keeps the 0.023 cap gap that rattles without jamming |

## 7. The complete test-lab table

Canonical minimal table (canon 5.8): lives at `/tables/test-lab/table.json`,
exercised by unit tests, and the first thing an author copies. It must match
this listing byte-for-byte (comments included).

```json
{
  // test-lab — minimal valid Tiltburst table. Canon: PLAN.md 5.8.
  // Used by tb_table unit tests and as the authoring starter.
  "format_version": 1,

  "meta": {
    "slug": "test-lab",
    "name": "Test Lab",
    "theme": "calibration chamber",
    "author": "Tiltburst",
    "description": "Minimal valid table used by unit tests and the authoring docs. One bumper, two targets, one top lane.",
    "rules_card": "Hit both targets to light the top lane. Top lane scores 5000."
  },

  "playfield": {
    "size": [0.52, 1.04],   // default table, meters
    "slope_deg": 6.5,
    "ball_count": 4
  },

  "elements": [

    // Outer boundary: rectangle with two rounded top corners (r = 0.10).
    // Closed => the final segment [0.520,0] -> [0.000,0] is implicit.
    { "id": "outer_wall", "type": "wall", "closed": true, "material": "wood",
      "path": [
        [0.000, 0.000],
        [0.000, 0.940],
        { "arc": { "to": [0.100, 1.040], "radius": 0.100, "dir": "cw" } },
        [0.420, 1.040],
        { "arc": { "to": [0.520, 0.940], "radius": 0.100, "dir": "cw" } },
        [0.520, 0.000]
      ]
    },

    // Lower guides funneling the ball to the flippers. Their lower ends
    // stop 16.1 mm from the flipper pivots -- 5.1 mm of surface gap past
    // the 0.011 pivot radius, well under the 0.024 blocking threshold, so
    // nothing squeezes past outboard of the flippers. Their upper ends sit
    // on the side walls at y = 0.270, which sets the narrowest passable
    // neck on the table: guide to the sling back corner [0.127, 0.190],
    // 0.0359 m. That is above the 0.033 minimum lane width and clear of
    // the 0.025-0.032 jam band (section 6). Raising these ends back to
    // y = 0.300 closes the neck to 0.0310 m -- inside the jam band.
    { "id": "left_drain_guide", "type": "wall", "material": "wood",
      "path": [[0.000, 0.270], [0.135, 0.130]] },
    { "id": "right_drain_guide", "type": "wall", "material": "wood",
      "path": [[0.480, 0.270], [0.345, 0.130]] },

    // One pop bumper mid-table (defaults: radius 0.031, kick 4.5 m/s).
    { "id": "pop_main", "type": "pop_bumper", "pos": [0.240, 0.760],
      "tags": ["scoring"] },

    // Two standup targets angled toward the playfield center.
    { "id": "target_left", "type": "standup_target", "pos": [0.085, 0.620],
      "facing_deg": -55, "tags": ["lab_targets"] },
    { "id": "target_right", "type": "standup_target", "pos": [0.395, 0.620],
      "facing_deg": -125, "tags": ["lab_targets"] },

    // Rollover in the top arch; the plunged ball crosses it.
    { "id": "top_lane", "type": "rollover", "pos": [0.240, 0.900],
      "facing_deg": 90 },

    // Drain sensor spanning the bottom, stopping short of the shooter
    // lane (x < 0.480) so it never captures the ball on the plunger.
    { "id": "drain", "type": "outhole",
      "region": { "a": [0.020, 0.010], "b": [0.460, 0.010] } },

    // Ball store. capacity >= playfield.ball_count (V009).
    { "id": "main_trough", "type": "trough", "capacity": 4 },

    // Two insert lights, addressed by rules.lua via tb.light_on(id).
    { "id": "light_pop", "type": "light", "pos": [0.240, 0.715],
      "shape": "circle", "color": "#ff9a00" },
    { "id": "light_top_lane", "type": "light", "pos": [0.240, 0.860],
      "shape": "arrow", "direction_deg": 90, "color": "#00e5ff" }
  ],

  "prefabs": [

    // Standard lower flippers. Expands to flippers_left_flipper and
    // flippers_right_flipper; pivots at [0.141, 0.115] / [0.339, 0.115],
    // tip gap 0.068 (section 5.1).
    { "id": "flippers", "prefab": "flipper_pair_standard",
      "pos": [0.240, 0.115] },

    // Right shooter lane. Expands to shooter_wall (x = 0.480, running from
    // y = 0 up to the gate line y = 0.888), shooter_top_post at
    // [0.472, 0.888] (tangent to the wall, out of the lane), shooter_gate
    // (one-way, y = 0.888, span [0.480, 0.888]-[0.520, 0.888], both
    // endpoints 0.000 m from a collider), and shooter_plunger at
    // [0.500, 0.030] (section 5.2).
    { "id": "shooter", "prefab": "plunger_lane",
      "lane_width": 0.040, "top_y": 0.880 },

    // Slingshots above the flippers. Expands to slings_left_wall,
    // slings_left_sling, slings_right_wall, slings_right_sling
    // (section 5.3).
    { "id": "slings", "prefab": "sling_pair", "pos": [0.240, 0.210] }
  ]
}
```

`tb_validate tables/test-lab` must exit 0 with **zero warnings** — this is a
CI gate (16-testing-ci.md). The clearances that gate holds, hand-computed on
the listing above (keep them true when editing it):

| Neck | Width | Verdict |
|---|---|---|
| Drain guide ↔ sling back corner `[0.127, 0.190]` (narrowest passable neck) | 0.0359 | ≥ 0.033, outside the jam band; ridge clearance 0.01795 > 0.0165 so V006 is silent |
| Drain guide ↔ sling bottom corner `[0.165, 0.175]` (inlane mouth) | 0.0528 | corridor widens toward the flipper |
| Sling bottom corner `[0.165, 0.175]` ↔ flipper base surface | 0.0537 | inlane feeds the flipper |
| Shooter lane, `[0.480, 0.520]`, end to end | 0.0400 | top post is tangent, takes nothing out of the lane |
| Flipper tip surfaces (centers 0.068 apart, tip radius 0.007) | 0.0540 | center drain possible but earned (V005 band) |
| Drain guide lower end ↔ flipper pivot surface (pivot `[0.140855, 0.115]`, base radius 0.011) | 0.0051 | ≤ 0.024: blocks cleanly, not passable, so V006 never looks at it |
| `shooter_gate` span endpoints → nearest collider | 0.0000 / 0.0000 | ≤ V014's 0.006 |

test-lab also omits both optional `meta` score keys on purpose, and both
omissions are load-bearing for tests: with no `meta.replay_score` it takes
the 5,000,000 default (section 2), and with no `meta.default_scores` its
high-score list starts **empty** — nothing is seeded, because there is no
built-in ladder (section 2, 11-game-framework.md §7). It likewise declares no
`physics` block, so every override in the section 2 `physics` table sits at
its 08-physics.md default.

The matching minimal `rules.lua` lives in
10-scripting.md §6 and `audio.json` in 12-audio.md §6.1; the matching minimal
`art.json` is section 7.1 below.

### 7.1 The matching test-lab `art.json`

The minimal TBArt companion (schema and palettes: 13-art-direction.md §2–§3).
It ships at `/tables/test-lab/art.json` and, like the listing above, must
match byte-for-byte (comments included):

```json
{
  // test-lab — minimal valid TBArt document. Canon: PLAN.md 5.8.
  // Schema: 13-art-direction.md §3; palette assignment: 13 §2.2.
  "palette": "midnight-chrome",

  "layers": [

    // Ground: one dark solid rect over the whole playfield. bg0 keeps the
    // ball path dark (luminance rule, 13-art-direction.md §2.3).
    { "name": "ground", "z": 0, "blend": "normal", "primitives": [
      { "kind": "rect", "transform": { "pos": [0.260, 0.520] },
        "w": 0.520, "h": 1.040, "fill": "bg0" }
    ] },

    // Inserts: one additive glow disc bound to the light_pop insert; its
    // fill and glow follow that light's live brightness (13 §3.2).
    { "name": "inserts", "z": 50, "blend": "additive", "primitives": [
      { "kind": "circle", "transform": { "pos": [0.240, 0.715] },
        "r": 0.012, "fill": "#ff9a00",
        "glow": { "radius": 0.010, "intensity": 1.4 },
        "light": "light_pop" }
    ] }
  ]
}
```

Art coordinates are table meters in the physics coordinate system, so the
disc sits exactly on `light_pop`'s `pos`. Everything else in test-lab renders
from debug/derived geometry; this file is the floor of a valid `art.json`,
not a style example.

## 8. Validation rules catalog

`tb_validate` (and the in-game loader, which runs the same checks) evaluates
every rule below against the **expanded** element list. Severity `error`
blocks loading; `warning` loads but must be fixed for shipped tables (CI runs
validation with `--strict`, which promotes warnings to failures —
16-testing-ci.md). Message templates use `{placeholders}`.

| Code | Checks | Severity | Message template |
|---|---|---|---|
| V001 | Ids unique across elements + expanded prefab children | error | `duplicate id '{id}' (also at {pointer})` |
| V002 | References resolve: `toy.art_ref` in art.json; `prefab` names known; string-literal element/light/light-group ids in rules.lua exist | error (art_ref, prefab); warning (rules.lua ids) | `unresolved reference '{ref}' in {context}` |
| V003 | Exactly one outer boundary (closed wall enclosing the largest area) exists and is watertight (algorithm below) | error | `outer boundary leaks near [{x}, {y}]` |
| V004 | All element geometry and `rest_zones` inside `[0,0]..playfield.size` | error | `'{id}' extends outside the playfield at [{x}, {y}]` |
| V005 | Lowest left-input / right-input flipper tip gap (tip-center distance at rest) in [0.054, 0.090]; recommended [0.062, 0.074] | error outside hard range; warning outside band | `flipper tip gap {gap} outside [{lo}, {hi}]` |
| V006 | No tight corridors: passable lane narrower than 0.033 (algorithm below) | warning | `lane near [{x}, {y}] is {w} wide (< 0.033)` |
| V007 | No unreachable playfield regions (flood fill from plunger, below) | warning; error if the flipper area is unreachable | `region of {area} m2 near [{x}, {y}] is unreachable from the plunger` |
| V008 | ≥ 1 outhole exists; its region is reachable and below the lowest flipper pivots; region does not cross the shooter lane | error | `outhole missing/unreachable/misplaced: {detail}` |
| V009 | Trough `capacity >= playfield.ball_count` | error | `trough capacity {c} < ball_count {n}` |
| V010 | Ramp `height_profile`: s strictly increasing, s0 = 0, sN = 1, z in [0, 0.15], slope ≤ 0.60, entry z matches entry surface | error | `ramp '{id}' height_profile invalid: {detail}` |
| V011 | Layer consistency: non-drop ramp ends within 0.005 of the destination surface z and (for layer 1) within 0.005 of a layer-1 wall opening; each vuk kicker matches exactly one ramp path end within 0.03 m of its `pos` (none or multiple = error); layer-1 regions enclosed except at ramp ends | error | `'{id}' does not connect to layer {n}: {detail}` |
| V012 | Magnet sanity: `strength` ≤ 5.0, `radius` in [0.03, 0.20], radius > ball radius, magnet not within 0.05 of the outhole region | warning | `magnet '{id}' {param} = {v} is outside sane range` |
| V013 | Drop bank target faces do not overlap (adjacent centers ≥ width + 0.002 along the bank line) | error | `bank '{id}' targets {i}/{j} overlap` |
| V014 | Gate orientation sane: a 0.030 ray from `pos` along `facing_deg` does not hit a collider (the allowed direction is not into a wall); gate span endpoints each within **0.006** of a collider (it closes a lane). **Throughout V014, *collider* means static solid geometry only — exactly section 8.1's blocking list** — so a `gate` (dynamic: it blocks only in a state it can leave at runtime, 08-physics.md §6.7) and a `spinner` plate (not solid, 08-physics.md §6.6) neither block a ray nor anchor a span endpoint. The `plunger_lane` prefab measures **0.000 m at both endpoints** — inner endpoint on `<id>_wall`'s top node (in the standalone form `<id>_top_post`'s surface is tangent there too, but the wall node carries the measurement, so the merged variant measures the same), outer endpoint on the boundary (section 5.2) | warning | `gate '{id}' {detail}` |
| V015 | At least one flipper with `input:"left"` and one with `input:"right"` | error | `no flipper bound to {input}` |
| V016 | Performance caps: ≤ 400 expanded elements; ≤ 2000 total path nodes | error | `{count} {thing} exceeds cap {cap}` |
| V017 | `type` is one of the 20 canon types | error | `unknown element type '{type}'` |
| V018 | Required parameter present | error | `'{id}' missing required '{param}'` |
| V019 | Parameter inside its hard range (section 2 blocks incl. every key of the section 2 `physics` table — the five scalars plus `physics.tilt.{warn,hard,abuse}` — section 4 element tables, section 5 prefab tables) and inside its derived relations (`physics.tilt.warn < physics.tilt.hard`; `orbit`'s `mouth_x <= top_radius - 0.010`, and `mouth_x >= plunger_lane.lane_width + 0.033` when the plunger feeds it); arcs feasible; paths non-self-intersecting; first path node a point | error | `'{id}'.{param} = {v} outside [{lo}, {hi}]` |
| V020 | Parameter inside its recommended band where one is defined | warning | `'{id}'.{param} = {v} outside recommended [{lo}, {hi}]` |
| V021 | Element `material` is `wood`/`steel`/`rubber`/`plastic`; `materials` map keys likewise (plus `flipper_rubber`); override fields (`restitution`, `friction_static`, `friction_kinetic`, `spin_transfer`) in range | error | `unknown material '{name}'` |
| V022 | `format_version` supported (== 1) | error | `format_version {v} unsupported (loader supports 1)` |
| V023 | Cardinality: exactly one plunger, exactly one trough | error | `table needs exactly one {type}, found {n}` |
| V024 | Element's `layer` allowed for its type (section 4.21) | error | `'{id}' ({type}) not allowed on layer {n}` |
| V025 | `meta.slug` equals the pack directory name | warning | `slug '{slug}' != directory '{dir}'` |
| V026 | No unknown keys anywhere in table.json | warning | `unknown key '{key}' at {pointer} (typo?)` |
| V027 | Light `color` is `#rrggbb` or a palette role present in art.json | warning | `light '{id}' color '{c}' unresolvable; will render white` |
| V028 | `meta.default_scores` shape: exactly 10 entries; `initials` exactly 3 glyphs from `A–Z 0–9 space`; scores positive integers, non-increasing | error | `meta.default_scores invalid: {detail}` |
| V029 | `meta.autoplay_bounds`: every key parses as the section 2 metric-path grammar (`dotted_path` or `shots[<id>].{attempts\|made\|rate}`) **and** names a metric that exists in the 14-authoring-guide.md §8.2 report schema (a `shots[<id>]` id must be a labeled shot); at least one of `min`/`max` present, both numeric, `min <= max`; `skill` present and ∈ {0, 1, 2} | error | `autoplay_bounds '{key}': {detail}` |
| V030 | `light_groups`: members exist, are `light` elements, unique within the group; group ids collide with no element id or other group id | error | `light group '{id}': {detail}` |
| V031 | Light `function` is one of the 13-art-direction.md §6 tags | warning | `light '{id}' function '{f}' unknown` |
| V032 | Rules lint: a scoring element (anything that can emit a scoring event — `coverage.total` in 14-authoring-guide.md §8.2) is never scored by `rules.lua` | warning | `'{id}' ({type}) is never scored by rules.lua` |
| V033 | Rules lint: a `light` element (or `light_groups` group) is never driven by `rules.lua` | warning | `light '{id}' is never driven by rules.lua` |
| V034 | `art.json` references resolve: palette name/roles, layer names and `z` values, primitive `light` bindings, decal prefab names (13-art-direction.md §3) | error | `art.json: unresolved {kind} reference '{ref}'` |
| V035 | Glow budget: at most 15 % of the playfield area emissive in the default (non-mode) state (14-authoring-guide.md §6) | warning | `glow budget {pct} % of playfield exceeds 15 %` |
| V036 | Ball-path contrast: lane-guide stroke contrast and `z < 100` fill luminance meet the 13-art-direction.md §2.3 table | warning | `'{ref}' contrast {v} below the 13-art-direction.md minimum` |
| V037 | `audio.json` patch references resolve: every `map` target and `tb.play_sound` id is a table patch, a table wav, or a built-in patch (12-audio.md §6, §7.1) | error | `unknown sound patch '{ref}' in {context}` |
| V038 | Every scoring event the rules emit has a mapped sound (12-audio.md §7.2) | warning | `scoring event '{event}' has no mapped sound` |
| V039 | Every music state the table uses has a song assignment (12-audio.md §9 reserved song ids) | warning | `music state '{state}' has no song assigned` |

V000–V031 are table-format checks the in-game loader also runs. V032–V039 are
the **authoring-loop** checks: they read `rules.lua`, `art.json`, and
`audio.json` alongside `table.json`, are `tb_validate`-only (the loader skips
them), and land at M15 with the authoring tooling — 14-authoring-guide.md §8.1
describes the authoring response per code group.

### 8.1 Occupancy grid (shared by V003, V006, V007)

All three spatial checks run on one rasterization per layer:

1. Grid over `[0,0]..playfield.size` **padded by one ball diameter (0.027 m,
   rounded up to 14 cells = 0.028 m) on every side**, cell size 0.002 m. On
   the default table that is `[-0.028, -0.028]..[0.548, 1.068]`, 288 × 548
   cells (unpadded it would be 260 × 520). The padding ring is empty space
   and exists so V003 has an exterior seed cell — without it the grid has no
   cell outside the boundary and the check is unimplementable.
2. For each cell center compute `clearance` = distance to the nearest
   collider on that layer (wall segments/arcs, closed-wall interiors treated
   as solid, posts, flipper sweep volumes at rest, bumper bodies, target
   faces, sling triangles, captive slots, kicker rims). Rollovers, lights,
   magnets, outholes, ramps (on layer 0 they fly above it), **`gate` flaps
   and `spinner` plates** do not block: a blocking gate is a *dynamic*
   collider (08-physics.md §6.7) and this rasterization carries only static
   geometry — V007 handles one-way direction in its own fill (below) —
   and a spinner is a trigger, not a solid collider (08-physics.md §6.6).
   V014 reads *collider* off this same split (section 8).
   **The outer boundary is excluded from the closed-wall-interior rule**:
   only *non-boundary* closed walls (sling triangles, islands, toy
   colliders) have solid interiors. The boundary is the largest-area closed
   wall (V003); treating its interior as solid would make the entire
   playfield solid, leave no passable cell, and escalate V007 to error on
   every table. It still blocks as a chain of segments and arcs, as any wall
   does.
3. A cell is **passable** if `clearance >= 0.0135` (ball radius). The
   **corridor width** at a passable cell is `2 * clearance`.

V003 (watertight): flood fill passable cells from the padded corner cell
`[-0.027, -0.027]`, which is outside the outer boundary by construction
(step 1) and passable (its clearance to the boundary is 0.027). If the fill
reaches any passable cell inside the boundary, the boundary leaks; report
the leak cell's position. Gaps smaller than the ball diameter do not leak
(the ball cannot exit) and pass.

V006 (tight corridors): corridor width is evaluated **only at clearance
ridge cells**, never at every passable cell. A passable cell is a **ridge**
cell when its `clearance` is a local maximum across the corridor — formally,
for at least one of the four sampling axes (x, y, and the two diagonals),
both neighbours along that axis have `clearance <= ` the cell's and at least
one is strictly smaller. Ridge cells approximate the medial axis; a cell
hugging a collider sits on a monotone clearance ramp (clearance strictly
rises going away from the surface) and is therefore never a ridge cell.
A ridge cell with `clearance < 0.0165` (half of 0.033) is *tight*; any
8-connected chain of tight ridge cells longer than 0.020 m that the V007
fill actually reaches is reported once, at the chain's midpoint, with
`w = 2 * clearance` at the narrowest cell of the chain.

Evaluating every passable cell instead is the trap this rule was rewritten
to avoid: the 3 mm band between `clearance` 0.0135 and 0.0165 exists along
**every** collider on the table, so a wall-hugging chain longer than 0.020 m
runs beside every wall and V006 would fire on every table — including
test-lab, which section 7 requires to validate with zero warnings.
Sampled ridge clearance is not exact medial-axis width; 2 mm resolution is
accurate enough at ball scale.

V007 (reachability): flood fill passable cells from the plunger `pos` cell.
Passable 4-connected regions not reached, with area > 0.001 m² (250 cells),
are reported. Cells the V003 exterior fill claimed — the padding ring and
anything else outside the boundary — are **excluded**: they are not
playfield regions, and reporting them would fail every table. One-way gates
count as passable in their allowed direction only. If the cell between the
flipper tips is not reached, escalate to error — the table cannot be
played.

## 9. Versioning and migration

- `format_version` is a single integer, currently **1**. The loader accepts
  exactly the versions it has migrations for; anything newer is V022.
- Adding a new **optional** field with a default does not bump the version.
  Renaming a field, changing a unit or a default's meaning, or adding a
  required field bumps it.
- When version 2 exists, migrations live in `/src/table/migrations.cpp` as
  pure functions `json migrate_v1_to_v2(json)` chained at load; the on-disk
  file is never rewritten by the loader. `tb_validate --migrate` rewrites a
  pack in place to the current version, preserving comments is *not*
  required (nlohmann drops them; acceptable for an explicit migrate step).
- Deprecated-but-migratable fields produce a V020-class warning naming the
  replacement.
- Shipped tables in `/tables/` are always kept at the current version.

## 10. Loader error reporting

Authoring LLMs fix what they can locate. Every loader/validator diagnostic
carries **file, JSON pointer, V-code, severity, message**:

```
tables/test-lab/table.json:/elements/3/radius [V019][error] 'pop_main'.radius = 0.5 outside [0.02, 0.045]
```

- The JSON pointer targets the offending value when it exists, else the
  nearest enclosing object. Diagnostics on prefab-generated elements point at
  the **prefab instance** (`/prefabs/2`) and name the generated child id in
  the message (`generated 'slings_left_sling': ...`).
- JSON syntax errors are reported before validation with the byte offset and
  line/column from nlohmann's exception, code `V000`:
  `table.json:214:7 [V000][error] syntax error: unexpected '}'`.
- `tb_validate <pack-dir>` prints one diagnostic per line (format above) to
  stdout. Exit codes: `0` no errors (warnings allowed), `2` errors present,
  `3` file/IO failure. `--strict`: warnings also exit 2. `--json`: emit
  instead a single JSON array for machine consumption:

```json
[ { "file": "tables/test-lab/table.json", "pointer": "/elements/3/radius",
    "code": "V019", "severity": "error",
    "message": "'pop_main'.radius = 0.5 outside [0.02, 0.045]" } ]
```

- The in-game loader logs the same diagnostics through `tb_core` logging
  (05-engine-core.md) and refuses to load a table with errors; the table
  select screen shows it grayed out with the first error.
- The authoring loop (14-authoring-guide.md) is: edit → `tb_validate --json`
  → fix every diagnostic → repeat until clean → `tb_autoplay`.

## 11. Common pitfalls

Mistakes an implementor or authoring LLM is most likely to make, with the
correct behavior:

1. **Wrong units.** Writing `"pos": [240, 115]` (millimeters) or pixel
   coordinates. Everything is meters; the whole table fits in 0.52 × 1.04.
   V004 catches this instantly — trust it.
2. **Y-axis direction.** +y points *away from the player*; "top of the
   table" is y ≈ 1.0, flippers are at y ≈ 0.115. Do not flip the axis to
   screen coordinates — the renderer handles projection (06-rendering.md).
3. **`facing_deg` semantics vary by element.** Standup/drop targets: outward
   face normal (points the way the face looks). Spinner and rollover: the
   ball's travel direction along the lane. Gate: the allowed pass direction.
   Kicker uses `eject_angle_deg`: the eject velocity direction (ignored by
   vuks, which eject into their matched ramp). Plunger uses
   `launch_angle_deg`: the lane/launch direction, default 90 — the launch is
   *not* hardwired to +y. Re-read the element's subsection instead of
   assuming.
4. **Tip gap measured wrong.** The canonical 0.068 m is between tip
   *centers* at rest, not between tip surfaces (that is 0.054 with default
   tip radii). V005 measures centers.
5. **Non-watertight boundary.** Forgetting `"closed": true` on the outer
   wall, or starting the shooter-lane wall at `y = 0.02` leaving a
   ball-sized hole at the bottom. Prefab `plunger_lane` starts its wall at
   `y = 0` for exactly this reason.
6. **Outhole across the shooter lane.** An outhole region reaching under the
   plunger captures the ball waiting to launch. Stop the region short of the
   lane wall (test-lab stops at x = 0.460 with the wall at 0.480).
7. **Arc ambiguity.** Assuming an arc can span more than 180°, or guessing
   the bend side. Arcs are minor-only; `dir` picks the bend. Chain two arcs
   for long curves; verify the corner cases against the worked example in
   section 3.
8. **Guessing prefab child ids.** Scripts must use the exact documented
   suffixes: `flippers_left_flipper`, `topl_lane_light_2` — never
   `flippers_left` or `topl_light2`. V002 flags unresolved rules.lua ids.
9. **Editing expanded elements.** Prefabs re-expand on every load; there is
   no "apply prefab" persistence. To customize a child, replace the prefab
   instance with its documented expansion as hand-placed primitives.
10. **Post gaps in the jam band.** Surface gaps of 0.025–0.032 m between
    posts (or any two colliders) wedge the ball. Choose ≤ 0.024 (blocks) or
    ≥ 0.033 (passes); see section 6. Do not lean on V006 here: it reports
    only *ridge* chains longer than 0.020 m (section 8.1), so a short
    jam-band pinch — a corner poking at a guide, a post beside a wall —
    validates clean and still eats balls. test-lab's guide-to-sling neck was
    exactly this: 0.0310 m, silent, wrong; it is 0.0359 m now.
11. **Restricted elements on layer 1.** `plunger`, `drop_target_bank`,
    `ball_lock`, `outhole`, `trough`, `ramp` are layer-0 only (V024). Upper
    playfields use walls, flippers, targets, kickers, etc.
12. **Denormalized `height_profile`.** `s` runs 0 → 1 over the whole path,
    strictly increasing, and the entry keyframe z must match the entry
    surface. Meters for `z`, not "percent height".
13. **Skipping validation because the JSON parses.** Parsing is the weakest
    check. A table is valid only when `tb_validate` is clean; CI enforces
    `--strict` on shipped tables.
14. **Inventing parameter names.** The registry here and in 08-physics.md is
    shared and fixed; `kick_speed` is not `kick_strength`, `pos` is not
    `position`. V026 warns on unknown keys — treat every one as a bug.
    `exit_layer` is the classic invention: a ramp's exit layer is *derived*
    from its final keyframe z (section 4.13), never written. The `physics`
    block is closed the same way: the section 2 table is the whole
    authorable set, so a `physics.*` name 08-physics.md mentions but
    section 2 does not list — or `"tilt.warn"` written as a dotted key
    instead of inside the `tilt` sub-object — parses fine, warns V026, and
    is silently ignored by the sim.
15. **Orbit-lane elements off the lane center.** An orbit hugs the boundary,
    so its lane center is `mouth_x/2` — 0.0375 on the left and
    `W - mouth_x/2` = 0.4825 on the right at the default `mouth_x` 0.075 the
    shipped tables use (section 5.5). A spinner at 0.0975 — the center you
    get by insetting from the guide wall, as if the lane had its own outer
    wall — sits outside the lane entirely and is never touched.
16. **Expecting a "full travel" flag on `switch_hit`.** The captive ball
    fires two *separate* events at two different times: `switch_hit` when
    the free ball strikes it, `captive_full_travel{id}` if and when the
    captive reaches the far end at ≥ 0.3 m/s (section 4.15). There is no
    payload flag to test.
17. **Bounds without a skill, or bounds on a derived number.**
    `meta.autoplay_bounds` entries require `skill` and must name a real
    report path (section 2's grammar); ratios like the skill-0/skill-2
    score spread are review-only targets and cannot be declared.
18. **Expecting a built-in high-score ladder.** Omitting
    `meta.default_scores` does not fall back to seeded defaults — no ladder
    exists anywhere in the framework. The list starts empty and the first
    ten games fill it (section 2, 11-game-framework.md §7). Shipped tables
    declare all 10 entries; test-lab deliberately declares none.

## 12. Done when

- [ ] Loader parses strict JSON with `//` comments via nlohmann
      `ignore_comments=true`; rejects trailing commas and non-UTF-8.
- [ ] Top-level schema (section 2) loads with all defaults applied; degree
      fields normalized to [-180, 180); material overrides merge over the
      08-physics.md §4.3 material rows; every key of the section 2 `physics`
      table — `rolling_resistance`, `restitution_falloff`,
      `restitution_soft`, `live_catch_window_ms`, `live_catch_factor`,
      `tilt.{warn,hard,abuse}` — reaches the sim as the 08-physics.md
      constant it overrides (μ_rr, `kFalloff`, `kSoft`, `kLiveCatchWindow`
      in seconds = ms/1000, `kLiveCatchFactor`, the §7.2 bob thresholds, the
      §7.3 abuse threshold), and each absence yields that constant's default
      (0.025, 0.12, 0.5, 0.050 s, 0.15, 0.055, 0.085, 1.2) — one unit test
      per key.
- [ ] V019 rejects every section 2 `physics` value out of its range and
      rejects `tilt.warn >= tilt.hard`; an unlisted `physics.*` name and a
      dotted `"tilt.warn"` key both raise V026 and change nothing in the sim.
- [ ] `meta.autoplay_bounds` parses per the section 2 metric-path grammar
      — dotted paths plus `shots[<id>].{attempts|made|rate}` — with a
      required `skill`; V029 rejects an unknown path, a missing skill, and
      `min > max`; a bound on a metric outside the report schema fails.
- [ ] All 20 element types load from JSON to sim descriptors with the exact
      parameter names of section 4 (shared registry with 08-physics.md), and
      defaults match the tables in section 4.
- [ ] Path parser handles point and arc nodes, enforces minor-arc + `dir`
      disambiguation, and reproduces the worked corner example of section 3
      (center [0.100, 0.940]) in a unit test.
- [ ] All 11 prefab generators expand with the exact child ids, order, and
      geometry formulas of section 5; a golden-file test snapshots each
      prefab's default expansion and fails on any drift. `orbit` honours
      `mouth_x` (lane center `mouth_x/2` = 0.0375 left, `W - mouth_x/2` =
      0.4825 right at the default 0.075) and emits exactly three children —
      the guide wall and the two `open`-gate entry switches, never an outer
      wall; `plunger_lane` puts its gate and its wall top on the same line
      `top_y + 0.008`.
- [ ] Prefab expansion is deterministic and happens before validation;
      children inherit `layer` and `tags`; diagnostics on children point at
      the prefab instance's JSON pointer.
- [ ] `/tables/test-lab/table.json` exists and matches section 7
      byte-for-byte, `/tables/test-lab/art.json` exists and matches section
      7.1 byte-for-byte, both load, and `tb_validate tables/test-lab --strict`
      exits 0 with **zero** diagnostics.
- [ ] A pack with no `meta.default_scores` (test-lab) loads clean and its
      high-score list starts **empty** — no ladder is seeded from anywhere —
      while a pack that declares the key seeds exactly its 10 entries.
- [ ] A geometry test asserts test-lab's section 7 clearance table to
      1e-4 m: narrowest passable neck 0.0359 m (drain guide ↔ sling back
      corner), shooter lane 0.0400 m, flipper tip surfaces 0.0540 m, guide
      end ↔ flipper pivot 0.0051 m, both `shooter_gate` endpoints 0.0000 m.
- [ ] Every rule V000–V039 is implemented with the listed severity (V032–V039
      land at M15 with the authoring tooling and are exercised through
      `tb_validate`, not the loader); each has a **pass** and a **fail**
      fixture *pack directory* `tests/fixtures/schema/<vcode>_{pass,fail}/`
      (16-testing-ci.md §2.2) holding whichever of `table.json`, `rules.lua`,
      `art.json`, `audio.json` the rule reads, the fail pack emitting exactly
      that code at its declared severity and aborting the load only for
      error-severity codes.
- [ ] The occupancy-grid checks (V003/V006/V007) run at 0.002 m resolution on
      the ball-diameter-padded grid (288 × 548 cells on the default table),
      exclude the outer boundary from the solid-interior rule, evaluate V006
      only at ridge cells, and complete in < 250 ms for a 400-element table
      on CI hardware.
- [ ] Diagnostic output matches section 10 exactly (line format, `--json`
      schema, exit codes 0/2/3, `--strict` promotion).
- [ ] `format_version` gate: version 1 loads, version 2 is rejected with
      V022 and a clean message.
- [ ] Loading the same pack twice yields an identical element list (order
      and values) — asserted by a determinism test.
