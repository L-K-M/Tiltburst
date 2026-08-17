# 15 — Launch Tables

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 08-physics.md, 09-table-format.md, 10-scripting.md,
11-game-framework.md, 12-audio.md, 13-art-direction.md, 14-authoring-guide.md.

This document is the complete design for the five shipped tables (canon §5.8).
The implementor builds each table's `table.json`, `rules.lua`, `art.json`, and
`audio.json` from this document alone, making zero design decisions. Sibling
docs own the mechanisms; this document supplies the table-specific values.

## 0. Conventions used by every table section

### 0.1 Space and units

All tables use the default play area **0.52 m × 1.04 m** (canon §5.3), origin
bottom-left, +x right, +y up-table; positions are `[x, y]` meters. Angles
(`facing_deg`, `eject_angle_deg`) are degrees CCW from +x per
09-table-format.md, whose registry owns all JSON parameter spellings; rosters
list only non-default values.

### 0.2 ASCII map legend (shared by all five maps)

```
==== wall   ()/\ curved guide   F flipper   f mini flipper   S slingshot
O pop bumper   [123] drop bank   T standup   * spinner   G one-way gate
-o- rollover   K kicker/scoop   M magnet   L ball lock   C captive ball
% ramp entrance   v ramp drop exit   P plunger   DD outhole/drain
```

Maps are schematic: positions are approximate (authoritative coordinates are in
the roster tables), portrait orientation, top of map = y 1.04. In particular
the orbit lane is always the channel between the **outer boundary** and the
single guide wall drawn inside it (§0.8); any space a map seems to show
outside that lane is drawing slack, not playfield — there is none.

### 0.3 Shot angle convention

Shot-map angles are the ball's direction of travel at the shot mouth, measured
from straight up-table: **0° = +y; positive = leaning left (player's view);
negative = leaning right.** "From LF/RF" names the flipper that makes the shot
cleanly; "UF"/"MF" = upper / mini flipper.

Every §x.4 angle is **computed, not estimated**: `angle = bearing − 90`, where
`bearing` is the pivot→aim-point bearing from the named flipper's `pos`. The
aim point is fixed per element type so the numbers are reproducible: orbit
mouth = the guide wall's lower node `[0.075, entry_y_left]` on the left and the
right mouth's aperture midpoint `[0.4625, 0.894]` (§0.8); ramp = its entrance
node; target bank / standup row = the row centre (a ball on the row's bearing
strikes the face anywhere along it); everything else = the element's `pos`.
The angle column is **computed per this definition, never estimated**:
recompute it whenever a pivot or an aim point moves, and never hand-adjust a
single row.

**Launch windows.** 09-table-format.md §4.3 sweeps `side` "right" from
`rest_angle_deg` to `rest_angle_deg − swing_deg` and `side` "left" the other
way, and the bat launches perpendicular to itself in the sweep direction, so a
flipper can only put a ball on bearings **`rest − 90` → `rest − 90 − swing`**
("right") or **`rest + 90` → `rest + 90 + swing`** ("left"). Every flipper's
window is stated in its §x.3 roster row (the standard pair in §0.4), and every
§x.4 row's angle lies inside its flipper's window — that is the shot map's
binding validity test.

**What the straight-ray model is worth (binding, read before trusting any
margin).** Every clearance number in this document — the §2.4 method, the
+0.003 m floor, every `⊥ dist` and every **ball margin** — comes from one
model: a **straight ray from the flipper's `pos` (its pivot) to the aim
point**, with the obstacle's radius and the 0.0135 m ball radius subtracted.
The model omits two terms, and **both are larger than the margins it
produces**:

- **Slope gravity curves the path.** The ray is straight; a real ball is a
  projectile under `g·sin 6.5°` = 1.1105 m/s² in the plane. Over a typical
  shot the droop is **≈ 5.6 mm for a 5 m/s shot across 0.50 m** and
  **≈ 10 mm for a 4 m/s shot across 0.55 m** — and it grows as the square of
  the distance and falls as the square of the speed, so slow long shots are
  worse still.
- **The ball does not leave the pivot.** It leaves along the **bat face**,
  up to a `length` (0.076 m on the §0.4 pair) out from `pos`, so the true
  launch point moves along the bat with contact position and the real path is
  a translated, slightly rotated copy of the pivot ray. Near-field obstacles
  — anything within ~0.15 m of a flipper — move by millimetres under this
  term alone.

Consequence, and it is binding on how these numbers are read: **any clearance
below roughly 10 mm is INDICATIVE, not decisive.** The straight-ray model is
a **screening tool for gross blockage** — it proves an element sits squarely
across a shot line, and a large negative margin is a real geometric fault
worth fixing on paper — but a small positive margin is **not** proof that the
shot is makeable, and a small negative one is not proof that it is not. The
authority that settles makeability is **measurement**: the M16/M17
`tb_autoplay` shot-rate run, `shots[<id>].rate` against the table's floor
(§0.7, 14-authoring-guide.md §8.2). Where the two disagree, the measurement
wins and the geometry is retuned by §0.7's steps — never the other way round.
Each per-table clearance record points back here.

### 0.4 Standard bottom (shared prefab instantiation)

Every table uses these prefab instances with these values unless its roster
overrides them. Prefab parameter spellings are owned by 09-table-format.md; the
values here are binding.

| Prefab | Binding values |
|---|---|
| `flipper_pair_standard` | `pos` `[0.240, 0.115]`, `tip_gap` 0.068, `length` 0.076, `rest_slope_deg` 31, `swing_deg` 52. Pivot separation `D = 0.068 + 2·0.076·cos 31° ≈ 0.198`, so the generated pivots are `[0.141, 0.115]` / `[0.339, 0.115]`; the 0.068 tip gap sits in the middle of V005's [0.054, 0.090] band. **Launch windows (§0.3, binding for all five shot maps):** LF `(−31, 52, left)` ⇒ **59° → 111°**, i.e. §0.3 angles **−31° … +21°**; RF `(−149, 52, right)` ⇒ **121° → 69°**, i.e. **+31° … −21°**. A shot outside its flipper's band is not a shot, however plausible the map looks |
| `sling_pair` | `pos` `[0.240, 0.210]`, `spread` 0.150, `face_length` 0.070, `tilt_deg` 22, `kick_speed` 3.5 |
| `inlane_outlane_pair` | one instance per `side` (`"left"` / `"right"`), `mirror_axis_x` 0.240, wall/divider points at the 09-table-format.md §5.4 defaults (outlane channel ≈ 0.038 m); the expansion supplies each lane's `rollover` and light |
| `plunger_lane` | `lane_width` 0.040 (inner wall x 0.480, lane x ∈ [0.480, 0.520]), `top_y` 0.880, `max_speed` 7.5; the expansion supplies the `plunger` at `[0.500, 0.030]` and the one-way exit `gate` above `top_y`. On all five tables this lane merges into the right orbit, so it takes 09 §5.2's **merged variant**: three children, **no `<id>_top_post`** (§0.8) |
| — | hand-placed primitives: `outhole` region `{a:[0.20,0.012], b:[0.32,0.012]}`; `trough` capacity 4 |

### 0.5 Economy conventions

Scoring tables follow the 14-authoring-guide.md economy template: **Award |
Base points | PF-mult? | Avg count/game | % of avg game score**. "PF-mult?
yes" = playfield multiplier applies; values are pre-multiplier; end-of-ball
bonus is never playfield-multiplied; percentages sum to 100 ± 5.

### 0.6 High scores and the replay threshold

**Ten** seeded entries per table (rank 1 = Grand Champion), written to
`table.json` `meta.default_scores`: exactly 10 `{initials, score}` entries,
`initials` exactly three uppercase ASCII letters, `score` a positive integer,
non-increasing down the array (09-table-format.md §2, else V028). Loaded per
11-game-framework.md §7 (per-table top 10, shown as two pages of 5 in attract).
The ladder is calibrated to the table's §x.7 `score.p50` target (its band
midpoint): rank 1 ≈ **5×** that value, rank 10 ≈ **1×** it, monotonically
between — so the board is reachable but the GC is not.

Every table also declares **`meta.replay_score`** (09-table-format.md §2,
consumed by 11-game-framework.md §3.3). The framework's 5,000,000 fallback is
below every table here, so an undeclared threshold would award a replay extra
ball in nearly every game and skew the §0.7 gated metrics. Each §x.5 sets it
at **1.4–1.8× that table's `score.p50` band midpoint**, rounded to the nearest
500,000 — i.e. roughly one game in six to eight earns it at skill 2.

### 0.7 Difficulty measurement and tuning procedure

All targets in the per-table "Difficulty targets" sections are measured with
the M15 `tb_autoplay` tool, whose CLI, skill profiles, and report schema are
normative in **14-authoring-guide.md §8.2** — this document only supplies
per-table numbers and never invents a flag or a metric name. The acceptance
suite per table is

```
tb_autoplay tables/<slug> --runs 500 --skill {0|1|2} --seed 1 --balls 3 \
            --report review/<slug>-s{0|1|2}.json
tb_autoplay tables/<slug> --runs 1 --skill 1 --seed 1 --seconds 300 \
            --report review/<slug>-coverage.json
```

i.e. three 500-run `--balls 3` sweeps (run index *i* seeds the RNG with
`--seed + i`, so `--seed 1 --runs 500` is the classic seeds 1..500) plus one
300 s coverage session. Binding for M16+ acceptance; neon-drift is
retro-fitted at M15.

**Two run counts, and only one of them binds.** `--runs 20` is the
*iteration* count an author reads while tuning (14 §5.1's EGS paragraph and
14 §11 item 2 / step 11): fast feedback, but a 20-run median moves several
percent between seeds. `--runs 500` is the **binding acceptance** count, and
it is the only count a §x.7 number, a `meta.autoplay_bounds` entry, or a
Done-when tick may be read from. Every run count in this document is 500
(the coverage session's `--runs 1` aside); quote the count next to the value
in `design.md` so it is never ambiguous which one a number came from.

**Skill and session shape per metric (14 §8.3 is the authority; this table
only mirrors it — it never reassigns a skill or a shape).** Every §x.7 number
is read exactly here, and every bound mirrored into `table.json`
`meta.autoplay_bounds` records that skill (`{min, max, skill}`,
09-table-format.md §2):

| §x.7 metric | Read at skill | Read from | Mirrored as a bound? |
|---|---|---|---|
| `ball_time_s.p50` | 1 | `--seconds 300` | yes |
| `drains.center` | 1 (of 14 §8.3's 0/1/2) | `--seconds 300` | yes, declared at skill 1 |
| `drains.outlane_share` | 1 | `--seconds 300` | yes |
| `coverage.share` | 1 | `--seconds 300` | yes |
| `stuck_balls` | all | `--seconds 300` | yes, declared at skill 1 |
| `shots[<id>].rate` | 2 (floor also checked at 0) | `--balls 3` | yes |
| `score.p50` | 2 | `--balls 3` | yes |
| `modes.started_per_game` | 1 | `--balls 3` | yes |
| `modes.multiball_reach_share` | 1 | `--balls 3` | yes |
| `modes.wizard_reach_share` | 2 | `--balls 3` | yes |
| `score.p90 ÷ score.p10` | 2 | `--balls 3` | **no — review-only** |

The suite runs exactly **one** `--seconds 300` session, at skill 1, so every
row 14 §8.3 assigns the 300 s shape is read there — including
`drains.center`, which 14 §8.3 lists as meaningful at skills 0/1/2 while
declaring its bound at skill 1. Reading it at 1 is a read site, not a
reassignment.

CI gates the two shapes in two jobs (14 §8.2, 16-testing-ci.md §2.8): the
`--seconds 300 --skill 1 --check-bounds` smoke gates the five 300 s rows —
`ball_time_s.p50`, `drains.center`, `drains.outlane_share`, `coverage.share`,
`stuck_balls` — and a separate `--balls 3` job gates the game-scoped rows at
the skills above, running skills **0, 1 and 2** (skill 0 because 14 §8.3
declares a bound-able `shots[<id>].rate` ≥ 0.02 floor there). The spread ratio
needs no bound — it is checked by hand at step 11 and recorded in `design.md`.
A §x.7 range that omits one of those five rows takes the 14 §8.3 row verbatim
(`ball_time_s.p50` 22–60 s, `drains.center` < 0.35, `drains.outlane_share`
0.15–0.30, `coverage.share` ≥ 0.80, `stuck_balls` 0); every declared bound
**narrows** its 14 §8.3 row and may never widen or contradict it. `tilts` is
not one of the five: 14 §8.3 gives it session shape *both* at skill 2, so it
rides the `--balls 3` job and no §x.7 here declares it.

Report fields used by §x.7 (14 §8.2 schema): `ball_time_s.p50`, `score.p50`,
the spread ratio `score.p90 ÷ score.p10` (the variance target — a lognormal
spread ratio of 4.6 ≈ a coefficient of variation of 0.65), `drains.center`
(a fraction, not a percent), `shots[<id>].rate` (shorthand for the `shots[]`
entry whose `id` is that labeled shot: `made ÷ attempts`),
`modes.started_per_game`, `modes.multiball_reach_share`,
`modes.wizard_reach_share`, and `stuck_balls` (must always be 0).

Tuning procedure when a target misses — adjust in this order, one step at a
time, re-running the full suite after each: (1) outlane gap ±0.003 m (max
±0.006 m total); (2) ball-save duration ±2 s (range 4–14 s); (3) slingshot
`kick_speed` ±0.3; (4) `slope` ±0.25° (range 6.0–7.0°); (5) shot-mouth posts
±0.004 m. Acceptance = all targets in range on one uninterrupted suite (all
three 500-run sweeps plus the coverage session, same `--seed`). Never fix a
*time* metric with scoring values or a *score* metric with geometry.

**Precedence: this list first, then 14 §8.4.** These five steps are the
**first resort** for any of the five tables shipped here, because they are
written against the one §0.4 standard bottom all five share — no per-metric
diagnosis needed, and step 4 (`slope`) has no §8.4 row at all. 14 §8.4's
per-metric matrix is the **general authoring tool** and owns everything
these five steps do not reach: any new table, every score / mode / coverage
/ shot-rate metric, and layout fixes such as re-angling a face that rejects
toward the drain (the fix 14 §10 actually applied to atomic-diner). Walk
this list first; move to §8.4's row for the missed metric once it is
exhausted, or immediately if the metric is not one these five steps move.

**Steps 1 and 3 do change shared standard-bottom values** — §0.4's outlane
channel and `sling_pair` `kick_speed` 3.5 — and that is allowed *here*, for
the one table being tuned. It is not a contradiction of 14 §10: a §8.4 step
never *invents* a §0.4 override (when a step needs a shared value the table
does not already override, you walk on down the row), whereas §0.7 steps 1
and 3 may *create* one, because a deliberate divergence from the shared
bottom is the whole point of the step. When one lands, §0.4's "unless its
roster overrides them" takes effect: record the new value as an explicit
override in that table's §x.3 roster and note it in `design.md`, so the
shared bottom remains the default for the other four.

### 0.8 Orbit instantiation and the top band (binding, all five tables)

All five tables instantiate the 09-table-format.md §5.5 `orbit` prefab with
the **same** parameters — the prefab defaults — and every table's outer
boundary uses the matching corner radius. **An orbit hugs the outer wall**:
the boundary *is* the lane's outer edge and the prefab's guide wall is the
lane's inner edge, so nothing lies outside an orbit lane.

| Param | Value | Why |
|---|---|---|
| `mouth_x` | **0.075** | x of the lane's *inner* edge — the guide wall — mirrored to `W - 0.075 = 0.445` on the right; these are the two mouths every §x.3 declares. With the outer edge on the boundary it is also the lane width |
| `top_radius` | **0.130** | 09 §5.5 feasibility `mouth_x <= top_radius - 0.010` (guide corner arcs r 0.055); the table's own outer wall must use the same radius (top corner arc centers `[0.130, 0.910]` / `[0.390, 0.910]`) |

0.075 is not slop: 09 §5.5 requires
`mouth_x >= plunger_lane.lane_width + 0.033 = 0.040 + 0.033 = 0.073` so the
right mouth clears the shooter wall by a full minimum lane width. The four
lines that follow from those numbers (W = 0.520, H = 1.040):

```
left lane   x ∈ [0.000, 0.075]   left lane center  x = 0.0375
right lane  x ∈ [0.445, 0.520]   right lane center x = 0.4825
guide (inner) wall top run       y = 1.040 − 0.075 = 0.965  (arcs r 0.055)
lane outer edge = the boundary   top run y = 1.040          (lane center y 1.0025)
```

**`entry_y_left` has a floor, and it is set by the right flipper (computed).**
The left orbit is shot from RF, and RF's launch window tops out at **121°**
(§0.4). To enter the left mouth the ball must pass *outboard* of the guide's
lower node `[0.075, entry_y_left]`, i.e. on a bearing of at least
`bearing(RF → node) + asin(0.0135 / d)`. **09 §5.5's prefab default 0.550 is
never usable on a table that shoots the left orbit**: it requires
`121.25 + 1.52` = **122.77°**, **1.77° past RF's rest end**, so every table
here overrides it. The requirement falls as the mouth rises, and
break-even — the mouth height at which the requirement equals 121° exactly — is
**0.58058** ⇒ `119.5547 + 1.4453` = 121.00000°. The authored values are
**0.620** on §1.3, §4.3 and §5.3 ⇒ `117.60 + 1.36` = 118.96°, **2.04°** of
*mouth* margin; **0.600** on §3.3 ⇒ 119.96°, **1.04°**; and **0.660** on §2.3
⇒ 117.12°, **3.88°**. Nothing may lower `entry_y_left` below **0.590** on a table
whose §x.4 shoots the left orbit — that authored floor sits 0.00942 above
break-even and still keeps **0.512°** of window (`119.0648 + 1.4235` =
120.48833°), so it is the floor, not the break-even point, and all five
authored values sit above it.

**Mouth clearance is necessary, not sufficient (binding, all five tables).**
Every number above tests **one point** — the guide's lower node — and says
nothing about the 0.55–0.60 m of field the ray crosses to reach it. An entry
band is usable only where the **whole ray** is clear by §2.4's method, so each
§x.4 must record the **controlling obstacle across the band**, at both ends,
not just the mouth geometry; a band whose field is blocked is worth only the
sub-band that is clear, and the shot row must say so. The worked example is
neon-drift's `pit_scoop`: a scoop at `[0.100, 0.480]` clears the 118.95683°
floor ray by +0.00491 but passes **0.01687** from RF's 121° rest-end ray
against the 0.0275 a 0.014 rim plus ball needs — **−0.01063** — so the rim
clears only for bearings ≤ **119.60270°** and **68.4 %** of a nominally
**2.04317°** band is dead (**0.64587°** usable) while the shot map still
reads unconditional. **Test every element on both roles**: an element on or
near an orbit ray is an *obstacle* to that shot as well as a *target* of its
own, and a scoop, a captive slot or a kicker rim must be cleared in both
roles before its §x.4 row is written. §1.3's `[0.180, 0.480]`, §2.3's
`shaker` and §3.3's `control_booth` are all sited on that test.

**Every element that lives in an orbit lane sits on that center line** —
0.0375 on the left, 0.4825 on the right. That includes the prefab's own
`<id>_left_switch` `[0.0375, entry_y_left + 0.05]` and `<id>_right_switch`
`[0.4825, entry_y_right + 0.02]` — `open` gates spanning the full 0.075 m
lane, so no ball can ride a wall past them (09 §5.5) — and every hand-placed
magnet, lock or scoop a roster puts in a lane. **A `spinner` is the one
exception**, and keep-out (c) below says why: its plate sits 0.0085 *outboard*
of the center line, x **0.029** on the left and **0.491** on the right, so that
its inboard end opens an escape gap instead of a pocket. There is **no strip
outside an orbit**: the boundary *is* the lane's outer wall, so there is no
"ordinary playfield outside the loop" band on any of these tables to place
anything in, and nothing outboard of a lane can trap a ball. Anything inside a
lane between `entry_y` and the top arc intercepts **every** ball that takes
that loop, so only elements meant to (a spinner, a magnet, and the two locks
that deliberately catch a loop — neon-drift's `drift_lock`) live there, each
with the rule that says what happens to a ball it did not want. An element
that wants every loop ball but must also fire *out* of the lane sits just
**below** the mouth instead, in open field (cosmic-carnival's
`cannon_breech`, in the right mouth's throat 0.018 under it). Everything else — mode scoops,
captive balls, standups — sits **below** its side's `entry_y`, and each
§x.3 raises `entry_y_left` where it must to keep it there.

**Plunge feed: the shooter lane merges into the right orbit.** Both hug the
boundary, so above the gate line they are one channel and no transition wall
exists (or is wanted — one would sit across the shooter lane's exit). 09
§5.2's `shooter_wall` stops at `[0.480, 0.888]` (`y_gate = top_y + 0.008`,
also the gate line); the orbit's guide wall begins at `[0.445, 0.900]`. Above
y 0.888 the lane is the boundary outside and the guide inside — **0.075 m
wide** — running from the plunger over the top and down the left leg to the
left mouth. The **right mouth** is the aperture between those two nodes,
`sqrt(0.035² + 0.012²) = 0.037 m`: passable (≥ 0.033), clear of the
0.025–0.032 jam band, and the way a flipper shot enters the right orbit and a
returning ball leaves it.

**The merged lane emits no top post.** 09 §5.2's `<id>_top_post` belongs to
the standalone form only, and its merged variant drops it, because at this
junction the post stands *in* the mouth: `[0.472, 0.888]` is **0.002595 m**
from the mouth line — inside its own 0.008 radius — so the disc straddles the
aperture and splits it into 0.000 against the wall node and
`sqrt(0.027² + 0.012²) − 0.008` = **0.021547 m** to the guide's lower node,
both under the 0.027 ball. With the post the right mouth passes **nothing**,
in either direction, on all five tables. Worse, against the closed
`orbit_merge_gate` flap it makes an upward-opening V that parks a ball at
centre `[0.486145, 0.904192]` — contact normals 48.86° (post) and 108° (flap)
bracket the 90° that has to carry gravity — a permanent stuck ball no
stuck-ball check can see move. So the wall simply ends at `[0.480, 0.888]`;
a segment collider's rounded end cap (08-physics.md's swept-circle-vs-segment
resolves the endpoint) already does the post's one job.

**Nothing else intrudes on the mouth.** The flap and the shooter gate both
*start* at that same wall node and run into the lane, away from the aperture;
the guide wall does not exist below y 0.900; the orbit's right entry switch
(y 0.920) is above it; and cosmic-carnival's `cannon_breech` centre is **0.017027 m** perpendicular *below* the
mouth line, so its 0.014 m rim stops 0.003 short of the aperture and sits in
the throat, where §4.3 wants it. No table places anything else within the
`[0.445, 0.900]`–`[0.480, 0.888]` span, so the aperture is the full node-to-node
**0.037 m**, with no radius to subtract.

One roster element makes the junction one-way. Every table places it, ids and
values identical:

| id | type | pos | params | notes |
|---|---|---|---|---|
| `orbit_merge_gate` | gate | `[0.500, 0.8945]` | `width` 0.042, `facing_deg` 108, `default_state` "one_way" | flap span `[0.480, 0.888]` → `[0.520, 0.901]`, i.e. 18° above +x; length `sqrt(0.040² + 0.013²)` = 0.0421. Inner endpoint is `shooter_wall`'s top node (V014 distance **0.000**; the merged lane has no top post, above), outer endpoint is on the boundary's straight right leg, below its corner tangent y 0.910 (**0.000**). The 0.030 m V014 ray from `pos` along 108° runs `[0.500, 0.8945]` → `[0.49073, 0.92303]`, inside the lane. One lane-spanning element crosses that corridor — the orbit's own `<id>_right_switch` at y 0.920 (ray x 0.49171, all five tables) — and it is **not a collider**: an `open` gate never collides (09 §5.5) and 09 §8.1's collider list omits it. The ray therefore hits nothing V014 counts. **Keep-out, binding, two rules:** (a) *V014*: no table may put a **solid** collider (wall, post, target face, kicker rim, bumper body) in the right lane between y 0.8945 and y 0.9231. (b) *Stuck balls*: no table may put a **spinner** anywhere in the merged right lane above the gate line y 0.888. A spinner is "a trigger plus a 1-D plate angular model, not a solid collider **(except when too slow to pass)**" (08 §6.6) — under `s_pass` 0.15 m/s the plate is a `steel` wall for that contact — and the whole straight leg (y 0.900 to the corner tangent at 0.910) lies inside the V014 corridor, so a plate there spans the lane at a height every plunge reaches. With the plate level (normal 90°, the one normal that carries gravity, above) a ball that crossed upward at just over 0.15 m/s re-crosses downward at `v − 0.12` < 0.15 and comes to rest **on** it; tilting it does not help, because a plate that spans its lane (Common pitfalls) makes a wedge with a lane wall whose two normals bracket 90°. There is therefore no valid spinner position in the right leg on any table; the only table with a plate near this junction, cosmic-carnival, carries it in the **left** leg (§4.3), where keep-out (c) below — not "off the plunge path", which the left leg is not — is what makes it safe |

**Keep-out (c), spinners on an orbit leg (binding, all five tables).** (b)
empties the merged **right** leg. The mirror case is the **left** leg, which
every `left_orbit` shot *climbs* (§1.4, §3.4, §4.4, §5.4) and which (b) never
reaches — a ball can cross a left-leg plate going **up** at any speed at all.
Same slow-pass wall, computed: with `facing_deg` φ a ball on the lane axis
crosses at `s_pass = v·sin φ`, so 08 §6.6 makes the plate a `steel` wall below
`0.15/sin φ`. A ball crossing **upward** at `v` leaves at `v − 0.12·sin φ`,
apexes `(v − 0.12 sin φ)²/(2·1.35420)` above the plate and returns at
`sqrt(0.86685/1.35420)` = **0.80008** of that, so it re-crosses fast enough to
pass only for `v ≥ 0.12 sin φ + 0.15/(0.80008 sin φ)` — **0.30855 m/s** at
φ 100, **0.30966** at φ 76, and never below **0.30748** (φ 90, its minimum).
**The floor is 0.31 m/s.** Under it the ball settles *on* the plate, and on the
lane center it stays there: the 0.025 m segment leaves
`(0.075 − 0.025·cos 14°)/2` = **0.02537 m** to each lane wall, under the 0.027 a
ball needs, so both ends are closed pockets — under a **12.675°** lean
(`asin(0.24367/1.11052)`, Common pitfalls' 12.7°) the ball never restarts at
all, and over one it slides to an end cap
and wedges, cap-and-wall normals **151.6°/0°** outboard or **28.4°/180°**
inboard, each pair bracketing the 90° that has to carry gravity. That is the
deleted top post's wedge again, so **facing alone cannot rescue a plate on the
lane center**, and neither can siting: a climbing ball reaches every height in
its own leg at every speed down to zero.

**The rule.** A spinner in an orbit lane sits **0.0085 outboard of the lane
center — x 0.029 left, 0.491 right — with `facing_deg` 76 (left) / 104
(right)**, and its roster row records its slowest crossing in each direction
that leg carries. The exactly-14° lean is **1.325°** past the 12.675° restart
threshold (gravity beats rolling resistance along the plate by **0.02499
m/s²**), and the offset opens the **inboard** gap to **0.03387 m** — passable
(≥ 0.033), clear of 09 §6's 0.025–0.032 jam band, wider than the 0.027 a ball
needs — so a ball that does stop restarts, slides inboard and **drops through
that gap** back down the lane and out the mouth. No pocket can form: the
plate's inboard cap stands 0.03387 from the guide face, so a ball can never
touch both. The outboard gap stays **0.01687**, blocked (≤ 0.024). The plate
still covers **0.04113 m** of the lane's 0.048 m of ball-center travel
(**85.7 %**); the inboard **0.00687 m** is the escape channel, and no authored
path is in it — a post-loop ball descends hugging x 0.0135, and an RF mouth
entry is at x **0.02091** by neon-drift's floor bearing 118.95683° at plate
height. A ball that does ride the channel simply misses that spin.

| Table | Spinner | pos | Slowest **descent** | Slowest **climb** | Margin on 0.31 |
|---|---|---|---|---|---|
| neon-drift | `speedo_spinner` | `[0.029, 0.690]` | **0.74228** | **1.15109** | +0.43228 / +0.84109 |
| atomic-diner | *(none)* | — | — | — | — |
| tilt-o-tron | `flywheel_spinner` | `[0.029, 0.700]` | **0.73051** | **1.13791** | +0.42051 / +0.82791 |
| cosmic-carnival | `plate_l` | `[0.029, 0.680]` | **0.68117** | **1.25355** | +0.37117 / +0.94355 |
| cosmic-carnival | `plate_r` | `[0.029, 0.860]` | **0.50622** | **0.89750** | +0.19622 / +0.58750 |
| cosmic-carnival | `hoop` | `[0.290, 0.600]` | not in a lane — both plate ends open on field | — | re-faced 92 → **76** |
| voltage-vandals | `fence_spinner` | `[0.029, 0.690]` | **0.74228** | **1.15109** | +0.43228 / +0.84109 |

Descents are the marginal loop (apex ball centre y 1.0265, speed 0) evaluated
at the plate, `sqrt(2[1.11052·Δh − 0.24367·s])` — `plate_l`'s figure has
`plate_r`'s **0.11644** axial loss (`0.12·sin 76°`) taken out first. Climbs are
the slowest ball that still finishes the loop from there: that same 0.11644
plus what it needs at that height to reach the apex and pay the 0.12671 top
run, chained through any plate above it. The tightest margin on any of the five
is `plate_r`'s **+0.196 m/s**, so no leg plate sits in the wall band on any path
its leg carries, and the 14.0° lean is the backstop for anything that does stop.

What it does, in the three directions a ball can take:

- **Up (plunge).** Velocity is +y; `(0,1)·dir(108°) = +0.951 > 0`, so the
  flap is transparent and the plunged ball rides straight on up the merged
  lane. It trips `orbit_merge_gate`'s `switch_hit`, and above y 0.920 the
  orbit's own right entry switch.
- **Down (orbit return, or a stalled plunge).** Any ball descending the right
  leg over the shooter lane — every ball whose center could otherwise reach
  the gate shelf — meets the closed face, and slides down the flap's **18°**
  incline, above the 12.7° at which slope gravity beats rolling resistance
  (08-physics.md §1.3), to the flap's lower end at `[0.480, 0.888]` — which is
  also `shooter_wall`'s top node, and, with the post gone, the *only* collider
  there. **Where it ends up, and why nothing holds it:** the wall's end cap is
  round, so around that node the outward normal turns continuously from the
  flap's **108°** to **180°**, the wall's playfield face — a 72° fan with no
  re-entrant corner anywhere in it. Holding a ball against gravity needs a
  contact normal at **90°**, and 90° is outside `[108°, 180°]` by 18°, so at
  every point of the fan gravity keeps a tangential component pointing
  down-left and the ball keeps turning. It leaves the fan at 180° with its
  centre on x = 0.4665 (0.0135 off the wall face, which is vertical, so gravity
  is tangent to it) and rolls down into open playfield above the right flipper —
  on cosmic-carnival, straight into `cannon_breech`. This is exactly the
  contact cone a `<id>_top_post` would break: its 48.86° with the flap's 108°
  *does* bracket 90°, and that V holds balls forever — which is why the merged
  variant emits no post. A ball inboard of the flap's lower end drops
  straight through the 0.037 m mouth instead. Either way it never reaches the
  closed shooter gate and never comes to rest at the junction.
- **At rest — the stuck-ball case, closed.** The shooter gate's shelf (ball
  center y = 0.888 + 0.0135 = 0.9015, x ∈ [0.4935, 0.5065]) lies entirely on
  the flap's *facing* side: signed distance +0.0067 m at x 0.500 and
  +0.0046 m at the boundary, both less than the 0.0135 m ball radius, so a
  ball there is already penetrating the flap and is pushed up-left onto it,
  then rolls off into the field. The wedge under the flap (legs 0.040 ×
  0.013) has an inscribed radius of 0.0055 m — smaller than the ball — so it
  is transit space for the plunge, never a pocket. **Nothing can come to rest
  on the closed gate.**

**Plunge thresholds.** With `slope` 6.5° and `rolling_resistance` 0.025 the
up-lane deceleration is `g·sin 6.5° + μ·g·cos 6.5° = 1.1105 + 0.2437 =
1.3542 m/s²` (08-physics.md §1.3); the plunger sits at `[0.500, 0.030]` with
`max_speed` 7.5:

| Plunge | Release speed | Outcome |
|---|---|---|
| dead | `v < 1.524` = `sqrt(2·1.3542·0.858)` | never reaches the gate line at y 0.888; rolls back down onto the plunger — re-plunge, no ball lost. **Four tables, not five:** on cosmic-carnival `feed_deflector` (§4.3, flap centre y 0.41928) catches the ball far lower, so only `v < 1.046` returns to the plunger there and **1.046 ≤ v < 1.524** slides the flap and exits `feed_gate` onto `cannonade_flipper` — a different shot, still not a lost ball |
| stub | `1.524 ≤ v < 1.5427` | passes the one-way shooter gate but not the merge flap: its apex ball-centre falls short of y 0.90869, so it settles back on the closed gate shelf, which the flap overlaps (the *at rest* case above), is pushed up-left onto the flap and off its lower end. Same exit as a soft plunge, but it never enters the orbit leg |
| **soft** | `1.5427 ≤ v < 1.6028` | clears the flap — at x 0.500 the flap face is at y 0.8945, so the ball centre must reach `0.8945 + 0.0135/cos 18°` = **0.90869**, i.e. `sqrt(2·1.3542·0.87869)` = 1.5427 — then stalls in the right leg or the top corner, comes back down and is turned out of the right mouth into open playfield — **this is the skill-shot feed** |
| grey | `1.6028 ≤ v < 1.6906` | reaches the guide's corner (ball-center apex y = 0.9785, i.e. `Δh` 0.9485 off the plunger ⇒ `sqrt(2·1.3542·0.9485)` = **1.60278**) and up to the boundary-arc apex, but cannot pay for the whole top run; falls back down the right leg, or dies on the top run — where the lane floor is the guide's horizontal top run (y 0.965, ball centre 0.9785) and only 11-game-framework.md §4.6's stuck-ball impulses get it moving again. **Never authored against**, and the reason no skill shot is keyed to a plunge above 21.4 % |
| **full** | `v ≥ 1.6906` | **completes** the loop. Reaching the boundary-arc apex (ball center y = 1.0265) is `Δy` 0.9965 over a **1.06300 m** path — the ball *centre* runs 0.0135 inside the boundary, so the straight run is `0.910 − 0.030` = **0.880** and the corner is a quarter of the centre's own arc, `0.1165·π/2` = **0.18300** — split the path on the *centre* path's tangent point, never on the boundary's, or both terms come out long ⇒ `sqrt(2[1.11052·0.9965 + 0.24367·1.06300])` = 1.65267 — but that speed arrives with *zero* left, and the 0.260 m horizontal top run still costs `2·0.24367·0.260` = 0.12671 of `v²`, so the threshold is `sqrt(1.65267² + 0.12671)` = **1.69057**. Rides the top and comes down the left leg, out the left mouth at `entry_y_left` |

Against `max_speed` 7.5 those are: **20.3 %** of plunger charge at 1.524,
20.6 % at 1.5427, 21.4 % at 1.6028 and **22.5 %** at 1.6906. The window that
matters to a player is the whole mouth feed — stub + soft, every plunge the
flap turns back out of the right mouth — **20.3–21.4 %**, 1.05 points wide,
≈ 15.8 ms of release timing at `charge_time_s` 1.5; the soft band proper is
20.6–21.4 % (0.80 points, ≈ 12 ms). A full plunge is anything past 22.5 %. Both
are reachable, so the skill shot is a real release-timing skill rather than a
coin flip.

**The top band.** `y = 0.965` — the guide wall's top run — is the ceiling of
the open playfield between x 0.130 and 0.390: every layer-0 element in that
band sits at **y ≤ 0.913**, so the headroom under the guide is 0.052 m,
comfortably passable (≥ 0.033) and nowhere near 09 §6's 0.025–0.032 jam band.
Consequences, binding for every roster below:

- `top_lanes_n` uses `lane_length` **0.050** and `pos.y` **0.880**: dividers
  span y 0.855–0.905 and their rubber caps reach 0.913. The bank is entered
  **from below** — a ball rolling up-table into a lane trips its rollover
  first; a fast one then crosses the 0.052 m headroom, meets the guide and
  drops back into the bank, possibly down a neighbouring lane, but the
  rollover it has already tripped is the one that scores. Each §x.5's "soft
  plunge into the lit lane" skill shot means: plunge into the mouth-feed
  window (1.524–1.6028 m/s — §0.8's stub or soft band, above),
  take the ball the merge flap turns out of the right mouth, and flip
  it up into the lit lane inside the skill window (see Common pitfalls; that
  window ignores the two orbit entry switches and `orbit_merge_gate`, all
  three of which the plunge itself trips).
- Pop clusters drop out of the band with them; each §x.3 lists its new
  centroid. Every pop-cap-to-neighbour gap is either ≤ 0.024 (blocked) or
  ≥ 0.033 (passable), never between.

### 0.9 Sound-brief vocabulary (§x.6 tables)

Each §x.6 row names a sound by its *trigger*. A row whose key is one of
12-audio.md §7.2's fixed **purpose** keys is authored as an `audio.json`
`"map"` entry (purpose → patch id, 12 §6) and needs no script; every other
row is a table sound played from `rules.lua` with `tb.play_sound(patch_id)`
at the moment the brief describes. The §x.6 tables spell purpose keys exactly
as §7.2 does — `flipper`, `slingshot`, `pop_bumper`, `standup_target`,
`drop_target`, `spinner`, `rollover`, `ramp_made`, `magnet`, `kicker`,
`launch`, `drain`, `tilt_warning`, `tilt`, `ball_lock`, `wall_hit`,
`ball_ball` — so a spinner step is keyed `spinner`, a falling drop target
`drop_target`, and a magnet energising `magnet`. Anything else (jackpot,
crane pulse, hatch purchase, act start…) is a `tb.play_sound` cue.

---

## 1. neon-drift — Neon Drift

### 1.1 Fantasy

Midnight street racing through a synthwave city. The game is momentum: shift
up through five gears with the drop bank, chain ramps like drifting through
corners, and let the "drift corner" magnet bend your orbit shots sideways
like a rear end breaking loose. The multiplier only holds while you keep
shooting.

### 1.2 Art

- **Palette:** `sunset-synth` (13-art-direction.md §2.2, binding assignment):
  deep indigo grounds (bg0 `#0D0221`), hot magenta primary (`#FF2975`),
  electric cyan secondary (`#08F7FE`), violet / taxi-yellow accents
  (`#9D4EFF` / `#FFD319`), amber `warm` reserved for warnings.
- **Motifs:** (1) horizon grid with sun-stripe circles receding up-table;
  (2) chrome speed-line chevrons along ramps and lanes; (3) tachometer arcs —
  the gear indicator is a five-segment tach around the gear bank.
- **Logo:** "NEON DRIFT" in italic outlined chrome caps with a magenta
  underline swoosh that trails into speed lines; cyan drop shadow.

### 1.3 Layout

```
+--------------------------------------+
|         _R___P___M_                  |  R P M top lanes (rollovers)
|       /(-o-)(-o-)(-o-)\              |
|     /                   \ __         |
|    / .---------.          \ G|       |  G: gate, plunger lane top
|   | / DRIFT     \    O      |;|      |  O: pop bumpers "traffic"
|   || CORNER      |  O  O    |;|      |
|   || M   [L]  v  |          |;|      |  M: drift magnet, L: drift_lock
|   | \ __________/           |;|      |  v: right-ramp drop exit
|   |*|                       |;|      |  *: speedo spinner (left orbit)
|   |;|        _____________  |;|      |
|   |;|       | 4  3  2  1 |  |;|      |  gear drop bank (faces SW)
|   |;|        -----------.   |;|      |
|   |;|   %L       %R      \ F3;|      |  ramp entrances; ramps CROSS
|   |(K)   \       /        \ ,|      |  mid-field on layer 1. F3 =
|   |;|     \     /   T T T  |;|      |  upper-right flipper. T: N-O-S
|   |;|      |   |           |;|      |
|   | /S\    |   |    /S\    |;|      |
|   | \_/  __|   |__  \_/    |;|      |
|   |-o-\ /         \ /-o-   |P|      |  in/outlane rollovers; P plunger
|   | \  \   F   F   /  / |  |'|      |
|    \ '  \ (L) (R) /  ' /            |
|     \____   DD   ______/            |  DD: drain
+--------------------------------------+
```

Prefabs: standard bottom (§0.4); `orbit` per **§0.8** (`mouth_x` 0.075,
`top_radius` 0.130, `entry_y_left` **0.620**,
`entry_y_right` 0.900) — mouths at x 0.075 / 0.445, lane centers **0.0375**
(left) / **0.4825** (right); the shooter lane merges into the right orbit
above y 0.888, with the §0.8 `orbit_merge_gate` at `[0.500, 0.8945]`;
`top_lanes_n` n=3 labels R-P-M `pos` `[0.220, 0.880]`, `lane_length` 0.050
(lane centers x 0.180 / 0.220 / 0.260); `pop_cluster` n=3 centroid
`[0.315, 0.790]` spacing 0.070 ⇒ `d` = 0.070/√3 = **0.040415** (09 §5.9), so
`pop_1` `[0.315, 0.830415]` (top cap y **0.861415**), `pop_2`
`[0.280, 0.769793]`, `pop_3` `[0.350, 0.769793]`.

The nest's surface gaps to its neighbours. The two `gear_bank` gaps are
**0.020082** and **0.037860** — measure each one separately and never collapse
them into a single figure; they are on opposite sides of 09 §6's jam band and
have opposite verdicts:

- `pop_2` → the `gear_bank` row's up-table target `[0.300, 0.715]`, nearest
  point its upper face end `[0.288296, 0.719389]`: **0.020082**. This is the
  **controlling** (tightest) gap, and it is **blocked by design** — the
  nest's lower-left shoulder seals against the top of the bank, so a ball
  climbing the middle cannot thread between the two and squirt out behind
  the bank; the pops take it and kick it back down. It sits **0.004918**
  below 09 §6's 0.025 jam-band edge, i.e. further than one §0.7 step-5 post
  move (±0.004 m), so no tuning step can walk it into the band.
- `pop_3` → the next target `[0.332, 0.703]`, nearest point
  `[0.325822, 0.705317]`: **0.037860** — **passable by design**, the nest's
  exit route down the bank's right side, ≥ 0.033 with **0.004860** of
  headroom over that minimum and 0.005860 above the band's 0.032 edge.
- `pop_1` → the R-P-M bank's nearest divider end `[0.280, 0.855]`:
  **0.011772** — blocked, as intended (the R-P-M bank is entered from below,
  §0.8, never from the nest).

Element roster (custom geometry). In every §x.3 roster the **notes** column
carries the derivation — why a value is what it is and what pins it — so a
reader who only needs the authored values should read the **id / type / pos /
key params** columns and skip notes until a value has to change.

| id | type | pos | key params | notes |
|---|---|---|---|---|
| `upper_flipper` | flipper | `[0.448, 0.615]` | `length` 0.062, `rest_angle_deg` **−100**, `swing_deg` 46, `side` "right", `input` "right" | cross-field backhand into the left ramp; fires with right button. **Launch window (computed, binding):** 09 §4.3 sweeps `side` "right" from `rest_angle_deg` to `rest_angle_deg − swing_deg`, −100° → −146°, and the bat launches perpendicular in the sweep direction ⇒ **170° → 124°**. The `left_ramp` entrance line from this pivot is **163.83°** at 0.3051 m — **6.17°** inside the rest end, 39.83° above the swing end — and the mouth sits **0.085 m above** the pivot, so the ball climbs. **The sign of this rest angle is load-bearing, and a positive one is a mirror error:** `rest_angle_deg` 148 sweeps 148° → 102° for launch bearings **58° → 12°** — up-*right* into `shooter_wall`, away from the ramp entirely. Two rules follow, and both are binding on any upper flipper: check the *sign* of `rest_angle_deg` against the bearing of the thing it feeds before authoring it, and site the pivot **below** that mouth — a pivot at `[0.435, 0.600]` would put the ramp entrance at bearing **188.13°**, **0.040 m below** it, unreachable by any flipper at any rest angle, and §1.4's "RF or UF" would have no UF. Pivot and mouth are placed together because neither position alone is enough: the gear-bank row runs across every flatter line, and this pivot's line clears `[0.396,0.679]`, the row's low end, by **0.04698** — that low end is the binding target, **not** the next one up at `[0.364,0.691]`, whose perpendicular is the larger 0.04959 — and the pop nest by **0.13101** to the centroid `[0.315,0.790]` — 0.07086 to `pop_2`'s 0.031 cap, which is the nest's nearest surface to this line. At rest the bat spans `[0.4372,0.5539]`–`[0.448,0.615]`, **0.0347** off `nos_targets`' near end and **0.021** off the shooter wall (blocked, ≤ 0.024); swept to −146° the tip reaches `[0.39660,0.58033]`, **0.03348** from the right-ramp mouth at its `[0.370,0.560]` — this gap is what caps that mouth's x at **0.370606** (§1.3 `right_ramp`) — passable, clear of 09 §6's jam band at both ends of the sweep |
| `left_ramp` | ramp | path `[0.155,0.700]` → arc up-left → across top mid → `[0.408,0.300]` | `height_profile` `[{"s":0,"z":0},{"s":0.35,"z":0.055},{"s":0.85,"z":0.045},{"s":1,"z":0.025}]`, `drop_exit` true | "climb"; exits onto right inlane |
| `right_ramp` | ramp | path `[0.370,0.560]` → arc up-left → `[0.0375,0.895]` (2-D length S ≈ 0.46) | `height_profile` `[{"s":0,"z":0},{"s":0.45,"z":0.052},{"s":0.80,"z":0.052},{"s":1,"z":0.030}]`, `drop_exit` true | "drop"; crosses left_ramp on layer 1 and drops into the left orbit lane above `drift_lock`. Grades (V010, abs(dz/ds) ≤ 0.60): 0.052/0.207 = **0.25**, 0, −0.022/0.092 = **−0.24** — and the profile ends at `s` 1 as V010 requires. **Entrance x 0.370, and the window is narrow (computed).** This is §4.4's family: the LF→right-mouth bearing is 67.574° on every table, so any right-ramp entrance in the x 0.35–0.38 band lands within a few mm of what a ramp mouth needs. Below x **0.36631** the mouth falls under §2.4's binding +0.003 floor against that ray — the perpendicular runs 0.926 m per m of x, so 0.365 gives 0.03729 against the 0.0355 a 0.022 half-width plus ball needs ⇒ **+0.00179**, 1.21 mm short, and §5.3's copy of the same problem is solved by moving 0.008 outboard. **0.008 is not available here:** at 0.373 `upper_flipper`'s swept tip `[0.39660, 0.58033]` sits **0.03113** from the mouth, inside 09 §6's 0.025–0.032 jam band, so the cap that keeps that gap passable (≥ 0.033) is x **0.370606**. The whole usable band is therefore **0.36631–0.370606**, 4.3 mm wide, and this row sits in it. At **0.370** the orbit ray clears by **+0.00642** — §5.3's +0.00612 class — the tip gap is **0.03348**, and the binding obstacle on that shot becomes the right mouth's own aperture nodes at +0.00497 (§1.4) |
| `drift_magnet` | magnet | `[0.0375, 0.880]` | `strength` 1.8, `radius` 0.110 | on the left orbit lane center (§0.8); bends and catches orbit shots when on |
| `drift_lock` | ball_lock | `[0.0375, 0.862]` | `capacity` 2, `style` "visible", `eject_angle_deg` 90, `eject_speed` 3.0 | on the lane center, fed by the right_ramp drop exit 0.033 above it; drawn as a magnet catch (art). The lane runs through it, so the sim captures every left-orbit ball (08 §6.14): when the lock is unlit the script calls `tb.release_lock("drift_lock", 1)` in its `ball_lock` handler and the +90° eject fires the ball on around the orbit (the mandatory unlit-lock pattern, 10-scripting.md) |
| `gear_bank` | drop_target_bank | 4 targets `[0.300,0.715]`, `[0.332,0.703]`, `[0.364,0.691]`, `[0.396,0.679]` | `facing_deg` **249**, `reset` "script" | gears 1–5. Explicit `targets` array (09 §4.8), so the three numbers must agree: the row spans `sqrt(0.096² + 0.036²)` = **0.102528** over 3 gaps ⇒ pitch **0.03418** — the pitch is the step **along the row**, never the per-target *x* step (0.032 here), and the face gap 0.03418 − 0.025 = 0.0092 is inside 09 §5.10's 0.004–0.020. Row bearing −20.6°, so the outward normal is **249.4°**, authored as 249 — the face looks back down-table at the left flipper that shoots it (§1.4, −19.6° ⇒ bearing 70.4°, 1.0° off the row's own normal); a facing taken from anything but the row's own perpendicular (235, say) misses by 14°. **This row's y is pinned by the `pop_2` gap and may not slide down-table.** At these coordinates that controlling gap is **0.020082** — blocked, and a decisive 0.004918 clear of 09 §6's 0.025 jam-band edge — while the `pop_3` gap is **0.037860**, passable. Translate the row 0.003 m down-table and the `pop_2` gap becomes 0.023044: still blocked, but only 0.002 under the band, i.e. one §0.7 step-5 post move from landing in it. Keep the two gaps as two numbers (§1.3 above); they are not interchangeable |
| `pit_scoop` | kicker | `[0.180, 0.480]` | `style` "scoop", `eject_angle_deg` -57, `eject_speed` 3.0 | mode start / wizard start. **x 0.180 is pinned by the left-orbit corridor, not by the scoop's own sight line (computed).** §0.8 makes the left orbit an RF shot across the band **118.95683°–121°**, and at y 0.480 that band's ball-centre corridor is x ∈ [**0.11969**, **0.13704**]. The scoop must sit clear of that corridor **as an obstacle**, on one side or the other — a scoop just under its low side (x ≈ 0.100) clears the floor ray by 0.03241 (+0.00491) but passes **0.01687** from the 121° rest-end ray against the 0.0275 a 0.014 rim plus ball needs (**−0.01063**), leaving only bearings ≤ **119.60270°** and killing **68.4 %** of the band. **Left is not available:** clearing the 121° ray on that side needs x ≤ **0.08760**, whose rim would reach x 0.0736 and cut the orbit guide at 0.075. So it sits on the far side of the corridor: x ≥ **0.16847** clears, x ≥ **0.17189** holds §2.4's +0.003 floor, and **0.180** clears the whole band — **+0.01009** at the 118.95683° floor, its worst point, rising to **+0.02420** at 121°, so all **2.04317°** are usable (§1.4). It also clears the lines it now sits nearer: RF→`left_ramp` (107.460°) by **+0.01466**, LF→pops (75.545°) by **+0.02585**, LF→`gear_bank` (70.421°) by **+0.05807**, and its rim keeps 0.091 of clear surface to the orbit guide — nothing in 09 §6's 0.025–0.032 jam band |
| `speedo_spinner` | spinner | `[0.029, 0.690]` | `facing_deg` **76** | in the left orbit lane, on §0.8 keep-out (c)'s spinner offset (0.0085 outboard of the 0.0375 lane center) so the plate's inboard end opens a 0.03387 m escape gap. Crossings: **0.74228 m/s** slowest descent, **1.15109 m/s** slowest climb, against the 0.31 m/s wall floor |
| `nos_targets` | standup_target ×3 | `[0.430,0.520]`, `[0.443,0.490]`, `[0.452,0.460]` | `facing_deg` 205 | N-O-S; lights Nitro Save |

(No hand-placed drift-corner guide wall: the "drift corner" **is** the upper
left run of the orbit lane — the boundary at x 0.000 and the prefab guide at
x 0.075 are the corner, `drift_magnet` sits on the lane center under it, and
art draws the chevrons. A second arc here would have made a 0.030 m corridor
inside 09 §6's jam band on top of the prefab guide.)

### 1.4 Shot map

| Shot | From | Angle | Path |
|---|---|---|---|
| left_orbit | RF | **+27.6°** | left orbit mouth, past spinner, around the top and down the merged right leg — `orbit_merge_gate` turns it out of the right mouth into the field (§0.8), never into the shooter lane. **Clearance across the whole entry band 118.95683°–121°**, as §0.8 requires: tightest is `pit_scoop` `[0.180,0.480]` (§1.3) at 0.03759 ⇒ **+0.01009** at the floor bearing (its worst point), 0.05170 ⇒ +0.02420 at 121°; next `drift_lock` 0.09785 ⇒ **+0.07785** at its 0.020 m capture radius (§2.4). All **2.04317°** of the band are usable |
| right_orbit | LF | **-22.4°** | right orbit mouth, around top, exits the left mouth — **only with `gear_bank`'s `[0.364,0.691]` and `[0.396,0.679]` targets down**: up, they sit **0.01361** and **0.02055** off this ray against the 0.026 a target face needs (§1.3). Down, the tightest is the right mouth's own aperture nodes `[0.445,0.900]` / `[0.480,0.888]` (§0.8), both at 0.01847 ⇒ **+0.00497**; the `right_ramp` mouth, moved to `[0.370,0.560]` (§1.3), is 0.04192 ⇒ **+0.00642** (it was **+0.00179** at 0.365, under §2.4's floor), then `pop_3` 0.05661 ⇒ +0.01211 |
| left_ramp | RF or UF | **+17.5°** (RF) / **+73.8°** (UF backhand) | climb ramp → right inlane |
| right_ramp | LF | **-27.2°** | drop ramp → drift corner (entrance `[0.370, 0.560]`, §1.3; bearing 62.769°) |
| gear_bank | LF | **-19.6°** | drop bank face |
| pit_scoop | **LF** | **-6.1°** | mode scoop at `[0.180, 0.480]` (§1.3), bearing **83.901°** — **24.90°** above LF's 59° rest end and **27.10°** below its 111° swing end. Nothing stands between the pivot and the scoop; past it the `left_ramp` mouth is **+0.01273** and `pop_2` +0.02415. From RF the scoop bears **113.539°** — inside RF's window, but **5.42°** under §0.8's 118.957° left-orbit entry floor, so it is not an RF *aim* point: it is where a flat RF orbit miss lands, dropping into the scoop rather than caroming off its rim. LF is the aimed shot |
| nos_targets | **RF** | **-15.3°** | standup trio — **not** LF: the row runs 47.97°–54.49° from the LF pivot, 4.5°–11.0° flatter than LF's 59° rest end, and **71.86°–77.34°** from RF, inside its window throughout |
| pops | LF or RF | **-14.5°** (LF) / **+2.0°** (RF) | up the middle into the nest at `[0.315, 0.790]` (the R-P-M bank sits above them and is entered from below, §0.8 — it does not feed the pops). **Both lines only with `gear_bank` down**: up, `[0.300,0.715]` sits **0.00420** off the LF ray, and `[0.332,0.703]` / `[0.300,0.715]` sit **0.01390** / **0.01766** off the RF ray, all against 0.026. Down, the tightest is `pit_scoop` `[0.180,0.480]` at **+0.02585** (LF) and the `right_ramp` mouth `[0.370,0.560]` at **+0.01129** (RF); the mouth is +0.07517 on the LF line. Both figures are functions of §1.3's `pit_scoop` x and `right_ramp` entrance x — recompute them if either moves |

Margins above are computed by §2.4's method and radii, and carry §0.3's error
bars: the sub-10 mm figures screen for blockage, they do not prove the shot —
`shots[<id>].rate` settles it. **Two rows — three
shot lines — are drop-dependent** and say so, because `gear_bank` stands across the middle of
the field on the §1.3 coordinates that the `pop_2` / `pop_3` nest gaps pin
from both sides: the row cannot translate without walking one of those gaps
into 09 §6's jam band, so the honest reading is that a bank standing in front
of the middle screens what is behind it. §1.5's gear timer is what makes "down"
a state the player can hold — and `left_orbit`, `left_ramp`, `right_ramp`,
`gear_bank`, `pit_scoop` and `nos_targets` are unconditional either way.

### 1.5 Rules

**Skill shot.** Soft plunge into the lit RPM lane (lit lane rotates with
flipper lane-change): 750,000 × consecutive-skill streak. **Super skill:** full
plunge (right orbit) then left ramp from the upper flipper within 3 s: 2,000,000.

**Gears (global multiplier).** Completing `gear_bank` shifts up one gear
(1→5); playfield multiplier = gear number. Each shift restarts a 40 s gear
timer; expiry downshifts one (never below 1); gear = 1 at ball start. Bank
resets 1.5 s after completion. Light show: tach arc fills amber→magenta per
gear; downshift blinks the lost segment 3×.

**Race modes** — lit by rolling all three RPM lanes; start at `pit_scoop`.
One at a time; progress persists across balls; each can be played once before
Redline.

| Mode | Timer | Tasks | Awards | Light show |
|---|---|---|---|---|
| Street Sprint | 35 s | left_ramp ×3 | 400,000/shot, +2,000,000 & +1 gear on completion | magenta arrows chase up left ramp |
| Overpass Duel | 35 s | alternate left_ramp / right_ramp ×4 | 500,000/shot, +2,500,000 completion | both ramp lanes strobe alternately |
| Night Circuit | 40 s | any orbit ×4; `drift_magnet` on for whole mode; a curved (magnet-bent) loop counts double | 350,000/loop, +2,000,000 completion | cyan chase around full orbit |

**Overdrive Multiball.** Locks lit by 3× left_ramp; lock via right_ramp
(drop exit → `drift_lock`, magnet catch animation). 2 locks, then left_orbit
through the drift corner starts 3-ball. Jackpots (500,000, PF-mult) on both
ramps; after 4, Super 3,000,000 at left_orbit while `drift_magnet` flings the
ball (250 ms pulse on entry). Locks shared/stealable per 11-game-framework.md.

**Combos ("Drift Chain").** Any two different major shots within 3 s:
+150,000 × chain length. A 4-chain = "Perfect Drift": +1,000,000.

**Wizard — Redline.** Qualify: gear 5 + all 3 race modes played + Overdrive
started once. Start at `pit_scoop`. Phase 1 "Qualifying" (45 s): all 6 major
shots lit once each, 750,000/shot; collect all → Phase 2 "Final Lap" (25 s):
ramps only, 1,500,000 each, both within 5 s → Phase 3 "Redline Frenzy"
(20 s, 3-ball): every switch 100,000 at gear 5. Finishing Phase 2 awards
10,000,000 "Checkered Flag" + gear locked at 5 for the ball. All progress
resets after.

**Ball save:** 8 s from `ball_launched`. **Nitro Save:** completing N-O-S
lights one outlane save (one-shot, either side).

**Extra balls:** (a) second Perfect Drift of the game; (b) first time reaching
gear 5.

Scoring table (per §0.5; avg game score target 8,000,000):

| Award | Base points | PF-mult? | Avg count/game | % of avg game |
|---|---|---|---|---|
| Switch/rollover base | 5,000 | yes | 220 | 14 |
| Spinner (per spin) | 8,000 | yes | 60 | 6 |
| Ramp made | 60,000 | yes | 14 | 11 |
| Gear shift (bank complete) | 250,000 | yes | 3.0 | 9 |
| Race mode shots | 350,000–500,000 | yes | 4.5 | 25 |
| Mode completion | 2,000,000–2,500,000 | no | 0.5 | 14 |
| MB jackpots | 500,000 | yes | 1.6 | 10 |
| Super jackpot | 3,000,000 | no | 0.12 | 4 |
| Drift Chain combos | 150,000 ×len | no | 3.0 | 6 |
| End-of-ball bonus | 12,000 ×gear ×switches/10 | no | 3 balls | 5 |
| Redline payoff | 10,000,000 | no | 0.03 | 4 |
| Skill shots | 750,000–2,000,000 | no | 1.2 | 12 |

`meta.replay_score` **12,000,000** (§0.6: 1.5 × the 8.0M `score.p50` band
midpoint). High score defaults (10 entries, §0.6; rank 1 = 5 × 8.0M, rank 10
= 8.0M): 1) 40,000,000 AXL · 2) 32,000,000 VNS
· 3) 26,000,000 RGE · 4) 20,000,000 MPH · 5) 15,000,000 NEO · 6) 13,000,000
RPM · 7) 11,500,000 GTO · 8) 10,000,000 DRV · 9) 9,000,000 LAP ·
10) 8,000,000 KMH.

### 1.6 Sound / music brief

Deltas from the 12-audio.md built-in event bank (patches defined in this
table's `audio.json`; unlisted events keep built-ins):

| Key (§0.9) | Where | Patch | Character |
|---|---|---|---|
| `flipper` | map | `nd_shift_clack` | built-in clack + 30 ms low square thump |
| `pop_bumper` | map | `nd_horn` | short detuned saw horn blip, 120 ms |
| `spinner` | map | `nd_tach` | rising square tick, pitch += 4% per spin, cap 2× |
| `magnet` | map | `nd_drift_skid` | filtered noise skid, 600 ms |
| `ramp_made` | map | `nd_boost` | white-noise whoosh + sine riser, 350 ms |
| gear shift (bank complete) | `tb.play_sound` | `nd_gearshift` | clunk + engine-rev sweep 200→900 Hz, 400 ms |
| jackpot | `tb.play_sound` | `nd_nitro_hit` | big saw stab + sub drop, 500 ms |

Music states (12-audio.md §9 reserved song ids): `attract` 92 BPM neon ballad;
`main` 118 BPM synthwave groove, sidechained pads; per-gear variation: +1
pattern layer per gear (arps at 3+, lead at 5); `mode` 128 BPM driving;
`multiball` 140 BPM double-time; `wizard` 150 BPM peak with alarm stabs;
`game_over` 4-bar engine-down sting, tach falling to idle.

### 1.7 Difficulty targets

`ball_time_s.p50` 32–44 s · `score.p50` 6.0M–10.0M · `score.p90 ÷ score.p10`
≤ 4.6 · `drains.center` ≤ 0.32 · `shots[left_ramp].rate` 0.18–0.30 ·
`shots[left_orbit].rate` 0.22–0.34 · `modes.started_per_game` 1.8–4.0 ·
`modes.multiball_reach_share` 0.25–0.45 · `modes.wizard_reach_share`
0.01–0.05 · `stuck_balls` 0.

### 1.8 Build notes

Built incrementally across milestones (canon §6):

- **M5 (greybox):** `table.json` with outer walls (top corner radius 0.130,
  §0.8), the `orbit` instance with its `mouth_x` 0.075 guide wall plus the §0.8
  `orbit_merge_gate`, inlane/outlane walls and posts, `flipper_pair_standard`,
  `upper_flipper`, `plunger_lane`, `outhole`+`trough`. Slingshot faces exist
  as plain rubber walls (no kick). No ramps, no bank, no magnet, no lights, no
  rules beyond auto-serve of the next ball on drain. Playable: launch, flip,
  drain.
- **M6:** slings become `slingshot` elements; add `pop_cluster`, `nos_targets`,
  RPM `rollover` lanes, in/outlane rollovers, plunger-lane `gate`,
  `speedo_spinner`.
- **M7:** add `pit_scoop`, `gear_bank`, trough/ball-save plumbing.
- **M8:** add `left_ramp`, `right_ramp` (layer 1 crossing), `drift_magnet`,
  `drift_lock`.
- **M9:** `rules.lua` v1 = the complete §1.5 ruleset; placeholder
  `tb.show_message` text for light shows.
- **M13:** full `art.json` (palette, motifs, inserts, particles: magenta ramp
  streaks, drift-corner spark burst); light shows as specced.
- **M14:** full `audio.json` music states + attract loop; M15 retro-fits the
  §1.7 targets.

### 1.9 Distinctiveness

Neon Drift is the flow table: three flippers, two crossing ramps, and a
timer-decaying multiplier make it about continuous motion and combo chains.
Its skill test is accuracy at speed, where the others reward setup
(tilt-o-tron), spatial control (atomic-diner), aimed timing
(cosmic-carnival), or risk management (voltage-vandals).

---

## 2. atomic-diner — Atomic Diner

### 2.1 Fantasy

A chrome-and-cherry googie diner floating in space. You work the room: spell
food words across the field, assemble orders, and deliver them at the order
window. The upper mini-playfield is the counter itself — a tiny flipper
flicking the ball past pie cases — and the captive ball is a milkshake
shaker you rattle until the blender bursts into multiball.

### 2.2 Art

- **Palette:** `atomic-teal` (13-art-direction.md §2.2, binding assignment):
  deep sea-teal grounds (bg0 `#06181D`), neon teal primary (`#1BE7D2`) for
  chrome trim and script tubing, cherry-coral secondary (`#F45D48`), lime /
  mustard accents (`#B4E33D` / `#FFD166`). "Cream" appears only as
  `glow_white` (`#EAFFF8`) halo cores and flashes — the field itself stays
  dark per 13 §1/§2.3 (ball-path luminance ≤ 0.40, validated).
- **Motifs:** (1) starburst/atomic orbital clocks with electron dots;
  (2) boomerang-pattern countertop trim along lanes; (3) neon script tubing
  frames around scoops and the counter.
- **Logo:** "Atomic Diner" in cursive neon script (cherry-coral tube,
  mint-cream `glow_white` halo) atop a teal boomerang sign with three
  orbiting electron dots.

### 2.3 Layout

```
+--------------------------------------+
|          __F___R___I__               |  F R I top lanes
|   ____ /(-o-)(-o-)(-o-)\             |
|  |COUNTER (layer 1)|     \ __        |
|  | T T T   (K2)    |       \G|       |  counter: P-I-E standups,
|  |  P I E    exit  |  O     |;|      |  K2 exit kicker, f mini flipper
|  |  f ____________/  O  O   |;|      |  O: soda-bubble pops
|  |__/                       |;|      |
|  |;|  __                    |;|      |
|  |;| |C |     %C____________/;|      |  C: milkshake captive ball
|  |;| |__|    /  counter ramp |;|     |  %C: ramp up to counter
|  |;|        /       (K)     |;|      |  K: order window scoop
|  |;|  T    /                |;|      |
|  |;|  T   |     T           |;|      |  left T's: B-U-R  right T's: G-E-R
|  |;|  T   |     T           |;|      |
|  | /S\    |     T   /S\     |;|      |
|  | \_/  __|      __ \_/     |;|      |
|  |-o-\ /  E      S  \ /-o-  |P|      |  E,S inlane rollovers
|  | \  \    F    F    /  / | |'|      |
|   \ '  \  (L)  (R)  /  ' /          |
|    \____    DD    ______/           |
+--------------------------------------+
```

Prefabs: standard bottom (§0.4); `orbit` per **§0.8** (`mouth_x` 0.075,
`top_radius` 0.130, `entry_y_left` **0.660** — above the
`shaker` slot so the captive ball stays out of the lane — `entry_y_right`
0.900) with the §0.8 merge (`orbit_merge_gate`); the loop is shot and scored on the left
only, its right leg being just the exit the ball leaves by; `top_lanes_n` n=3
labels F-R-I `pos` `[0.300, 0.880]`, `lane_length` 0.050 (lane centers x
0.260 / 0.300 / 0.340); `pop_cluster` n=3 centroid `[0.335, 0.770]` spacing
0.072 (top cap y 0.843, 0.012 under the F-R-I bank — blocked).
`playfield.layer1_z` **0.048**, the z both counter ramps end at (V011).

Element roster:

| id | type | pos | key params | notes |
|---|---|---|---|---|
| `counter_floor` | walls, layer 1 | region x 0.055–0.335, y 0.762–1.000 | outline on `layer` 1, closed except at the two ramp-end openings (`counter_ramp` end and the `counter_drop` chute — V011). Front wall = two segments meeting at the 0.036 m chute opening: `[0.055,0.815] → [0.217,0.762]` (**18.1°**) and `[0.335,0.789] → [0.253,0.762]` (**18.2°**) | the counter mini-playfield. 18° is not decoration: a ball resting against a wall only slides along it when `1.1105·sin θ` beats the 0.2437 m/s² rolling resistance of 08-physics.md §1.3, i.e. above **12.7°**; a front wall at a token slope (1.5°, say) parks balls on layer 1 forever |
| `counter_flipper` | flipper | `[0.125, 0.815]` | `length` 0.052, `radius_base` 0.009, `rest_angle_deg` -28, `swing_deg` 48, `side` "left", `input` "left", `strength` 0.7, `layer` 1 | shoots up-right at PIE; pivot sits 0.023 above the front wall's left segment. **Launch window (computed):** `side` "left" sweeps −28° → +20°, launching perpendicular in the sweep direction ⇒ **62° → 110°**. `pie_targets` bear **97.59° / 81.12° / 64.98°** (P/I/E) from this pivot, all inside it; E is the tight one at **2.98°** off the rest end, P has 12.41° at the swing end |
| `pie_targets` | standup_target ×3 | `[0.105,0.965]`, `[0.150,0.975]`, `[0.195,0.965]` | `facing_deg` 275, `layer` 1 | P-I-E |
| `counter_exit` | kicker | `[0.290, 0.930]` | `style` "saucer", `eject_angle_deg` -80, `eject_speed` 2.4, `layer` 1 | collects made shots; ejects down-counter into the `counter_ramp`'s upper end so the ball rides the ramp back to layer 0 (ramp ends are the only legal layer exits, 08-physics.md §6.11) |
| `counter_drop` | ramp | path `[0.235,0.672]` → `[0.235,0.762]` (length **0.090**) | `height_profile` `[{"s":0,"z":0},{"s":1,"z":0.048}]`, `drop_exit` false | return chute. Grade `dz/ds` = 0.048/0.090 = **0.533** ≤ V010's 0.60; the 0.090 m path length is what keeps it legal — shorten it to 0.062 m for the same 0.048 rise and the grade is 0.774, a V010 failure. Its upper end meets a 0.036 m opening in the counter's front wall and matches `layer1_z` (V011); a layer-1 ball rolling down-table crosses that seam ≈ 0° off the into-path tangent, so it binds and rolls out at the layer-0 end at ≈ 1.0 m/s |
| `counter_hood` | wall | path `[0.198,0.650]` → `[0.270,0.632]` | `material` "wood" | the chute's deflector, **0.03125** below the layer-0 seam and 0.017–0.019 wider than it on both sides. It is what makes the chute un-backdoorable in 08-physics.md §6.10.2's terms: a ball travelling up-table from either flipper meets the hood and never reaches the seam, and the only remaining approaches are lateral (round the hood's ends), where the angle between `v` and the +y into-path tangent is ≈ 90° — far outside the 50° alignment gate — so such a ball crosses the seam **without binding**. **The lean is 14.0°, and the east node's y is what sets it (computed).** `dy` 0.018 over `dx` 0.072 is `atan` = **14.0362°**, net **+0.02499 m/s²** down-slope — above the **12.675°** at which `1.11052·sin θ` beats 08 §1.3's 0.24367 m/s² rolling resistance. Raise that node to `[0.270, 0.634]` and the lean is 0.016/0.072 = **12.5288°**, net −0.00277 m/s²: a *moving* ball still rolls off, so the deflection reads fine in a shot test, but a ball that **stops** on the hood can never restart, against `stuck_balls` 0. Any near-level deflector fails the same way; this is the threshold that also sets the 18.1° counter front wall, the 15.4° `tent_wall` and the 18° merge flap. The west node does not move, so `shaker`'s 0.03553 surface gap (row below) is unchanged |
| `counter_ramp` | ramp | path `[0.360,0.545]` → arc up-right → `[0.300,0.775]` | `height_profile` `[{"s":0,"z":0},{"s":1,"z":0.048}]`, `drop_exit` false | ends on layer 1 counter |
| `shaker` | captive_ball | slot `{a:[0.150,0.560], b:[0.150,0.640]}` | — | milkshake shaker; face at y 0.560, far end `b` at 0.640 — the end whose arrival at ≥ 0.3 m/s emits `captive_full_travel{"shaker"}` (§2.5). The slot top clears the left orbit mouth (`entry_y_left` 0.660) by 0.007, so the captive ball never sits in the lane. **x 0.150 is pinned by the left-orbit corridor (computed).** §0.8 makes the left orbit an RF shot on an entry bearing of **117.12°–121°**, and every ray in that band crosses x 0.085 between y 0.538 and y 0.611 — so the slot may not sit at the left wall. At x 0.085 the flattest legal ray (117.12°) passes **0.02322** from the resting captive centre against the 0.027 a ball plus a 0.0135 captive needs (**−0.0038**, worse toward 121°) and crosses the slot channel outright; only bearings ≤ 116.4° clear the ball, and those miss the mouth. On the far side of the corridor both tests pass: at 117.12° the ray passes **0.03464** from the slot at every point (**+0.00764**), and the clearance only grows toward 121°. The slot's upper end is 0.03553 of clear surface from `counter_hood`'s west node — passable (≥ 0.033), outside 09 §6's 0.025–0.032 jam band |
| `order_window` | kicker | `[0.415, 0.560]` | `style` "scoop", `eject_angle_deg` 237, `eject_speed` 3.0 | order delivery / mode start |
| `bur_targets` | standup_target ×3 | `[0.070,0.470]`, `[0.070,0.435]`, `[0.070,0.400]` | `facing_deg` 0 | B-U-R, left wall bank |
| `ger_targets` | standup_target ×3 | `[0.328,0.416]`, `[0.339,0.384]`, `[0.349,0.352]` | `facing_deg` 195 | G-E-R, center-right. **Threaded between two rays, and it may not move either way (computed).** The row runs down the 0.035 m channel between the LF→`counter_ramp` ray (bearing 63.010°) and the RF→`order_window` ray (80.308°): G clears the ramp line by **+0.00403** and R clears the scoop line by **+0.00404**, the two tightest numbers on this table. Translate it 0.031 m back along −26.99° (to `[0.300,0.430]` …) and G sits **0.00127** from the ramp ray against the 0.026 a 0.025-wide face plus a ball needs — and a standup never drops, so that would block the table's hardest shot permanently. The row's steps `[+0.011,−0.032]` / `[+0.010,−0.032]` and its 195° perpendicular are properties of the row itself and survive any pure translation; the clearances do not |

(No hand-placed `left_orbit_guide`: the `orbit` instance already emits both
walls of the left lane, and a second guide beside the prefab's would stand
0.030 m off it — a corridor squarely inside 09 §6's 0.025–0.032 jam band.)

### 2.4 Shot map

| Shot | From | Angle | Path |
|---|---|---|---|
| left_orbit | RF | **+25.8°** | left orbit mouth, around the top and down the merged right leg — `orbit_merge_gate` turns it out of the **right** mouth into open playfield (§0.8), never into the shooter lane and never into the F-R-I bank, which §0.8 enters from below |
| counter_ramp | LF | **-27.0°** | up to the counter (layer 1) |
| shaker | RF | **+23.0°** | captive ball face at `[0.150, 0.560]` (§2.3), bearing 113.01° — **7.99°** inside RF's rest end |
| order_window | **RF** | **-9.7°** | scoop — **not** LF: `[0.415, 0.560]` is 58.38° from the LF pivot, 0.62° flatter than its 59° rest end; from RF it is 80.31° |
| bur_targets | **LF** | **+12.5°** | left wall standups — **not** RF: the row runs 127.15°–133.35° from RF, 6.2°–12.4° past its 121° rest end, and 101.31°–104.00° from LF |
| ger_targets | **RF** | **0.0°** | center standups at the §2.3 coordinates — **not** LF: the row runs 48.73°–58.15° from LF, entirely below its 59° rest end; from RF it is 87.58°–92.09°, near-vertical |
| pie_targets | MF (counter) | **+7.6° / -8.9° / -25.0°** (P/I/E) | only from `counter_flipper`, window 62°→110° (§2.3) |

**Clearance record (computed, binding — the method every §x.4 uses, including
the inline margins in §1.4 and §4.4; read §0.3's "What the straight-ray model
is worth" first — every number below inherits those error bars).** For each row, take the ray from the named flipper's pivot on
the tested bearing, find every element it passes, and take the perpendicular
distance from the ray to that element's nearest surface less the 0.0135 ball
radius. A standup or drop target needs its 0.0125 half-width + 0.0135 =
**0.026**; a captive ball 0.0135 + 0.0135 = **0.027**; a kicker rim 0.014 +
0.0135 = **0.0275**; a pop cap 0.031 + 0.0135 = **0.0445**; a ramp mouth its
0.022 half-width + 0.0135 = **0.0355**; a bare wall or divider node 0.0135;
a **`ball_lock`** its capture radius **0.020 — with no ball radius added**,
because 08 §6.14's capture region is a circle of radius 0.02 m tested against
the ball **centre**, not a rim the ball's surface has to clear. A lock is
therefore never measured with the 0.0275 kicker figure or the 0.0135 bare-node
one; every lock in §1.4, §4.3 and §5.4 is measured at 0.020, and the tightest
of the four is voltage-vandals' `vault_lock` on the RF `vault_scoop` line at
**+0.00918**.
**The floor is +0.003 m.** Magnets, rollovers, lights and spinners are not
colliders (09 §8.1) and are not counted; neither are the §0.4 `sling_pair`
triangles, whose bottom corners are 0.06 m from the pivot and flank the launch
point itself — the ball leaves the flipper above and inboard of them, on every
table. An orbit row is tested at the **entry bearing §0.8 requires**, not at
the bearing of the aim node, and across that whole band.

| Shot | Tested bearing | Tightest obstacle | ⊥ dist | Need | **Ball margin** |
|---|---|---|---|---|---|
| left_orbit | 117.12° (§0.8 floor; band to 121°) | `shaker` slot `[0.150, 0.560–0.640]` | 0.03464 | 0.027 | **+0.00764** |
| left_orbit | 121° (RF rest end) | `bur_targets` B `[0.070, 0.470]` | 0.04774 | 0.026 | **+0.02174** |
| counter_ramp | 63.010° | `ger_targets` G `[0.328, 0.416]` | 0.03003 | 0.026 | **+0.00403** |
| shaker | 113.012° | `ger_targets` R `[0.349, 0.352]` | 0.10185 | 0.026 | **+0.07585** |
| order_window | 80.308° | `ger_targets` R `[0.349, 0.352]` | 0.03004 | 0.026 | **+0.00404** |
| bur_targets | 102.510° | `ger_targets` G — nothing nearer | 0.24776 | 0.026 | **+0.22176** |
| ger_targets | 90.000° | nothing on the line | — | — | **clear** |
| pie_targets (I) | 81.119°, layer 1 | `pie_targets` P `[0.105, 0.965]` | 0.04292 | 0.026 | **+0.01692** |

The two 4 mm rows — `counter_ramp` past G and `order_window` past R — are the
squeeze §2.3's `ger_targets` note pins from both ends; move that row either
way by 2 mm and one of them goes under the floor. Both sit far inside §0.3's
indicative band, so they are a warning to keep that row still, not a promise
that either shot lands: `shots.counter_ramp.rate` and `shots.order_window.rate`
in the §0.7 suite are what decide it.

### 2.5 Rules

**Skill shot.** Soft plunge to the lit F-R-I lane: 500,000 + spots one FRIES
letter. **Super skill:** full plunge, then `counter_ramp` within 4 s of the
ball reaching the flippers: 1,500,000.

**Food words.** BURGER = 6 standups (B-U-R left bank + G-E-R center bank,
letters latch). FRIES = F-R-I top lanes + E-S inlane rollovers (lane-change on
top lanes only). SHAKE = 5 **`captive_full_travel{id="shaker"}`** events — the
canon event (PLAN.md §5.7) the sim emits when the captive ball reaches the far
end `b` of its slot at ≥ 0.3 m/s. Every strike also emits the ordinary
`switch_hit{id="shaker", speed}` that pays the 40,000 shaker-hit row and feeds
the 30-hit extra ball; only full travel spells a letter, and no flag on
`switch_hit` is read. Completed
words hold until consumed by an order. Light show per letter: the letter
insert pops on with a starburst particle; word completion runs a marquee chase
around its lane group.

**Orders (mode ladder).** The current order shows on the backglass: Order 1
= BURGER; Order 2 = BURGER+FRIES; Order 3 = all three words. Completing the
order's words lights `order_window`; shooting it starts **Order Up!** (25 s):
4 lit delivery shots (left_orbit, counter_ramp, shaker, ger_targets) at
300,000 each; all 4 = delivery, 1,500,000 × order number, consumes the
words, advances the ladder. Expiry keeps word progress; re-deliver. Light
show: lit shots blink cherry-coral (secondary); delivery = `glow_white`
flashbulb ×3.

**The counter.** `counter_ramp` sends the ball to layer 1. Each PIE target
150,000; completing P-I-E = "Pie Case" 750,000 and +1 tip multiplier
(counter scoring ×n, max ×3, resets at ball end). `counter_exit` collects the
ball and sends it back down the `counter_ramp`; misses roll out through the
`counter_drop` chute to mid-field.

**Milkshake Multiball.** With SHAKE complete, each further
`captive_full_travel{shaker}` fills the blender 1/3 (light ring); at 3/3,
`order_window` starts 2-ball MB.
Jackpots: shaker 400,000; `counter_ramp` 600,000. Super: P-I-E during MB,
then shaker = 2,000,000. Blender empties at MB end.

**Combos ("Curb Service").** 3 different shots, each within 2.5 s of the
last: 400,000.

**Wizard — Dinner Rush.** Qualify: deliver Orders 1–3 + start Milkshake MB.
Start at `order_window`. Phase 1 "Rush Hour" (40 s): word switches score 3×,
all 6 major shots lit at 500,000, collect any 8 → Phase 2 "Kitchen Chaos"
(2-ball): `counter_ramp` jackpots 1,000,000, shaker relights → Phase 3 "Last
Call" (15 s): one shot to `order_window` = 8,000,000. Food progress resets
after.

**Ball save:** 10 s. **Comp Meal:** completing FRIES twice lights one outlane
save.

**Extra balls:** (a) deliver 2 orders during one ball; (b) 30 cumulative
shaker hits in a game.

Scoring table (avg game score target 6,000,000):

| Award | Base points | PF-mult? | Avg count/game | % of avg game |
|---|---|---|---|---|
| Switch/rollover base | 4,000 | yes | 240 | 16 |
| Letter awards | 25,000 | yes | 30 | 13 |
| Shaker hit | 40,000 | yes | 12 | 8 |
| Counter: PIE target / Pie Case | 150,000 / 750,000 | yes | 4 / 0.5 | 16 |
| Order Up! delivery shots | 300,000 | yes | 3.5 | 18 |
| Order delivered | 1,500,000 ×order | no | 0.45 | 10 |
| MB jackpots | 400,000–600,000 | yes | 1.2 | 9 |
| Super jackpot | 2,000,000 | no | 0.08 | 3 |
| Combos | 400,000 | no | 0.5 | 3 |
| End-of-ball bonus | 10,000 ×letters | no | 3 balls | 3 |
| Dinner Rush payoff | 8,000,000 | no | 0.02 | 3 |
| Skill shots | 500,000–1,500,000 | no | 1.4 | 8 |

No global playfield multiplier on this table: "PF-mult? yes" rows respond only
to the 2× playfield period awarded for 12 pop hits in a ball (60 s, one per
ball). `meta.replay_score` **9,000,000** (§0.6: 1.5 × the 6.0M `score.p50`
band midpoint). High score defaults (10 entries, §0.6; rank 1 = 5 × 6.0M,
rank 10 = 6.0M): 1) 30,000,000 CHF ·
2) 24,000,000 PIE · 3) 19,000,000 MLK · 4) 15,000,000 FRY · 5) 12,000,000 EGG
· 6) 10,500,000 MLT · 7) 9,500,000 BUN · 8) 8,500,000 TIP · 9) 7,000,000 SDA
· 10) 6,000,000 JOE.

### 2.6 Sound / music brief

| Key (§0.9) | Where | Patch | Character |
|---|---|---|---|
| `pop_bumper` | map | `ad_bubble` | sine blip + bandpassed fizz, random pitch ±3 semitones (tb.rng) |
| `ramp_made` | map | `ad_doorchime` | 3-note major chime (the counter ramp) |
| `drain` | map | `ad_plate` | plate-clatter noise burst |
| shaker hit | `tb.play_sound` | `ad_shake` | maraca noise burst + glass clink, 250 ms |
| letter collect | `tb.play_sound` | `ad_bell` | counter service bell "ding", 400 ms decay |
| order delivered | `tb.play_sound` | `ad_orderup` | two-tone bell + crowd-murmur noise swell |
| jackpot | `tb.play_sound` | `ad_blender` | rising blender whirr + pop, 600 ms |

Music (12-audio.md §9 reserved song ids): `attract` 96 BPM doo-wop progression
(I–vi–IV–V); `main` 132 BPM space-age bachelor-pad swing with walking bass;
`mode` adds hand-clap backbeat; `multiball` 152 BPM rockabilly double-time;
`wizard` 160 BPM frantic swing with klaxon accents; `game_over` 2-bar
closing-time vibraphone tag.

### 2.7 Difficulty targets

`ball_time_s.p50` 40–55 s · `score.p50` 4.5M–7.5M · `score.p90 ÷ score.p10`
≤ 3.7 · `drains.center` ≤ 0.28 · `shots[counter_ramp].rate` 0.15–0.26 ·
`shots[order_window].rate` 0.20–0.32 · `modes.started_per_game` 2.0–4.0 ·
`modes.multiball_reach_share` 0.30–0.50 · `modes.wizard_reach_share`
0.02–0.06 · `stuck_balls` 0 (watch the counter: a ball at rest on layer 1
must always reach `counter_flipper` or the `counter_drop` chute — gravity is
identical on all layers (08-physics.md §6.11), so the counter's front-wall
segments are authored at **18°**, above the 12.7° at which slope gravity
overcomes rolling resistance (§2.3), draining every rest position toward the
chute opening or the flipper).

### 2.8 Build notes

Built complete in **M16** as the authoring-pipeline dogfood: author strictly
via 14-authoring-guide.md workflow (design → `tb_validate` → greybox autoplay
→ rules → art → audio → §2.7 acceptance). Any friction found in the guide is
fixed in the same PR. Requires M8 layers (counter), M7 captive ball, M15
tooling.

### 2.9 Distinctiveness

Atomic Diner is the collection table and the easiest of the five: long ball
times, generous saves, and progress that accrues from almost every switch. It
is the only table with a true upper mini-playfield and the only one whose
modes are built from spelling collections rather than shot sequences — pace
is set by the player, not by timers, so it teaches the game's vocabulary.

---

## 3. tilt-o-tron — Tilt-O-Tron

### 3.1 Fantasy

A clanking retro-futurist assembly line building a giant robot one part at a
time. Three drop banks are the head, torso, and legs; flatten all three and
the magnet crane whirs across the ceiling, physically carrying your locked
ball to the assembly bay. Deliberate, mechanical pinball — until Full
Assembly drops four balls on you at once.

### 3.2 Art

- **Palette:** `arcade-purple` (13-art-direction.md §2.2, binding
  assignment): deep violet grounds (bg0 `#120521`), ultraviolet purple
  primary (`#B14EFF`), signal mint-teal secondary (`#00FFC6`), warning-red /
  steel-blue accents (`#FF3864` / `#3D9BFF`), hazard orange as `warm`
  (`#FF8E3C`, functional uses only).
- **Motifs:** (1) riveted steel plates (bg1 panels, purple rivet outlines)
  with hazard chevron tape in accent1 red along walls; (2) blueprint
  linework (mint-teal secondary on bg1) for the robot schematic
  center-field, parts filling in solid purple as banks complete; (3) toothed
  gears and piston arms framing the banks.
- **Logo:** "TILT-O-TRON" in blocky riveted slab capitals, ultraviolet
  purple with a mint-teal border, a red robot eye (accent1) dotting the
  hyphen; `glow_white` steam puffs.

### 3.3 Layout

```
+--------------------------------------+
|   ____________________________       |  the orbit's top run closes the
|  /   _O___I___L_                     |  field at y 0.965 (§0.8)
| |   (-o-)(-o-)(-o-)                  |  O I L top lanes, entered from below
| |;| L ______________________         |  L: assembly_bay lock, at the rail's
| |;|bay|  m     m     m   (K)|  |;|   |     west mouth; m: crane magnets ×3
| |;|   |______________ ___|__|  |;|   |  K: crane_dock, UNDER the rail's
| |;|        [ 1 2 3 ]              |;||     feed gap (kicks up into it)
| |;|         head                  |;||  head bank (faces S)
|   |*|                    % |    |;|  |  *: flywheel spinner
|   |;|                    g |    |;|  |  %g: gantry ramp entrance
|   |;|   m    m    m      a_|    |;|  |  conveyor magnets m ×3
|   |(K2)                  m p    |;|  |  K2: control booth scoop
|   |;|                [ 1 2 3 4 ]|;|  |  torso bank (faces SW)
|   |;| [ 1 2 3 ]                 |;|  |
|   |;|   legs        T T         |;|  |  T: weld standups
|   | /S\                /S\      |;|  |
|   | \_/  __        __  \_/      |;|  |
|   |-o-\ /  \      /  \ /-o-     |P|  |
|   | \  \    F    F    /  / |    |'|  |
|    \ '  \  (L)  (R)  /  ' /         |
|     \____    DD    ______/          |
+--------------------------------------+
```

Prefabs: standard bottom (§0.4); `orbit` per **§0.8** (`mouth_x` 0.075,
`top_radius` 0.130, `entry_y_left` **0.600** — above
`control_booth`, so the scoop sits in open field below the mouth —
`entry_y_right` 0.900) with the §0.8 merge (`orbit_merge_gate`); mouths at x 0.075 /
0.445, lane centers 0.0375 / 0.4825; `top_lanes_n` n=3 labels O-I-L `pos`
`[0.260, 0.880]`, `lane_length` 0.050 (lane centers x 0.220 / 0.260 / 0.300).
No pop cluster — intentional (see §3.9). The crane rail therefore lives in
the band **below** the O-I-L bank and **inboard** of both orbit legs
(x 0.165–0.385, roof ≤ 0.845), which is what fixes its coordinates below.

Element roster:

| id | type | pos | key params | notes |
|---|---|---|---|---|
| `head_bank` | drop_target_bank | 3 targets centered `[0.260, 0.790]`, pitch 0.034 | `facing_deg` 270, `reset` "script" | robot head; faces down-table, 0.016 under the crane rail floor (blocked, not a jam gap) |
| `torso_bank` | drop_target_bank | 4 targets `[0.310,0.560]`, `[0.3394,0.543]`, `[0.3688,0.526]`, `[0.3982,0.509]` | `facing_deg` 240, `reset` "script" | robot torso. **The row is derived from its `facing_deg`, not the reverse:** with 240 authored, the endpoints must lie on that facing's perpendicular from the west end — step `[+0.0294, −0.0170]` ⇒ pitch **0.03396** (face gap 0.0090) and row bearing **−30.0°**, whose outward normal is 240.0°, back down-table at the left flipper that shoots it. A row running *up* to the right instead (east end `[0.412,0.596]`) has outward normal 289° and faces no flipper at all, whatever its `facing_deg` says (§3.4, −26.9° ⇒ bearing 63.07°, 3.1° off the row's own normal). West end unchanged, so `crane_dock`'s clearance below is unaffected. **This row screens three other shots, and §3.4 says so.** It is 0.102 m long and sits ⊥ to LF's sight line at 0.47 m, so it subtends LF bearings **56.87°–69.20°** — 12.33° of that flipper's 52° window. `gantry_ramp` (63.74°) and `right_orbit` (67.57°) fall inside it and are struck at 0.01143 and 0.01355 against 0.026; RF→`head_bank` (96.68°) grazes the west end at 0.02293. No translation fixes this — shifting the row along either its own axis or its normal moves its angular span by under a degree — and the ramp mouth cannot clear it either, since the first bearing that does (72.73°) is `crane_dock`'s. All three are therefore **drop-dependent**, which is the honest reading of a bank that stands in front of a ramp: with the row down its targets stop colliding and each line opens (margins in §3.4). §3.5's "bank stays down" on completion is what makes that a state the player holds |
| `legs_bank` | drop_target_bank | 3 targets `[0.075,0.360]`, `[0.105,0.375]`, `[0.135,0.390]` | `facing_deg` **297**, `reset` "script" | robot legs, at the bottom of the robot. Span `sqrt(0.060² + 0.030²)` = 0.067082 over 2 gaps ⇒ pitch **0.03354** (face gap 0.0085), row bearing +26.6°, outward normal **296.6°** (authored as 297) — all three are properties of the row's own shape and survive a pure translation. **Its y is what does not (computed).** This row must sit clear of the only channel the RF up-left shots have: at `[0.085–0.145, 0.470–0.500]` it lies straight across it, with the §0.8 left-orbit entry band (119.96°–121°) passing **0.00924** from the middle target and **0.02425** from the top one against the 0.026 they need, and the RF→`control_booth` ray passing **0.01150** from the middle target — both of §3.4's RF shots blocked by a bank that is *up* at every ball start. At the authored y, 0.110 lower, the row clears the orbit band by **+0.00723** at its worst point (121°, top target) and the booth line by **+0.06064**. The **shooter** is the *left* flipper, emphatically: from RF the three targets bear 137.14°/131.99°/126.57°, all three past its 121° rest end, while from LF they bear 105.08°/97.88°/91.25°, comfortably inside (§3.4, +7.9°). The ball arrives 18.7° off this face's normal — well inside the ±90° that makes it a face hit |
| `crane_dock` | kicker | `[0.350, 0.786]` | `style` "scoop", `capture_ms` 1200, `eject_angle_deg` **120**, `eject_speed` 1.2 | lock pickup point, 0.022 m **below** the rail floor under its feed gap; the kick lobs the ball up-and-west through the gap into the channel (it is not an along-rail eject — that would fire into the underside of the floor wall). Reached from LF at −17.3° (bearing 72.70°, 13.70° inside LF's window). Its clearance is set by `torso_bank`'s west end: the perpendicular from the ray to `[0.310, 0.560]` is **0.02902**, less the 0.026 a target face plus ball needs ⇒ ball margin **+0.00302** — measure it that way, never centre-to-centre with nothing taken out (which reads 0.016 and means nothing). This is the tightest row on this table and the reason neither the dock nor that target may move toward the other |
| `crane_rail` | wall ×3 | floor `[0.385,0.809]` → `[0.372,0.8086]` **and** `[0.310,0.8070]` → `[0.165,0.803]`; roof `[0.385,0.845]` → `[0.165,0.839]` | `material` "wood" | channel under the crane, open at both ends, **0.036 m** clear everywhere (ball 0.027 + 0.009 play): above 09 §6's 0.033 minimum lane width and outside its 0.025–0.032 jam band — narrow this channel at any point (0.028, say) and it lands inside that band. Floor tilt toward the bay = atan(0.006/0.220) = **1.56°**. The 0.062 m break in the floor is `crane_dock`'s feed gap; nothing rolls across it (every magnet is west of it) |
| `crane_mag_a/b/c` | magnet ×3 | `[0.300,0.820]`, `[0.225,0.818]`, `[0.150,0.816]` | `strength` 1.6, `radius` **0.090**, `default_on` false | on the ball-centre line of the rail (floor + 0.0135). Spaced **0.075 < radius**, so each magnet's field reaches the previous one's core — that is what makes the hand-off chain work — and the three fields cover x ∈ [0.060, 0.390], i.e. the whole channel and the bay |
| `assembly_bay` | ball_lock | `[0.150, 0.816]` | `capacity` 3, `style` "visible" | at `crane_mag_c`'s core, 0.015 past the rail's west mouth, so a ball held by the last magnet is in the lock mouth. 0.075 inboard of the left orbit guide (x 0.075), so orbit balls can never enter it. Re-run at 08 §6.14's **0.020 m** capture radius (§2.4) the tightest §3.4 line is `control_booth` at 0.08625 ⇒ **+0.06625**; every other row is ≥ +0.07107 |
| `conv_1/2/3` | magnet ×3 | `[0.160,0.580]`, `[0.250,0.580]`, `[0.340,0.580]` | `strength` 0.5, `radius` 0.060, `default_on` false | conveyor |
| `control_booth` | kicker | `[0.160, 0.560]` | `style` "scoop", `eject_angle_deg` -55, `eject_speed` 3.0 | mode start. **x 0.160, and the test that fixes it is lateral, not vertical (computed).** Sitting below the left orbit mouth is *not* the test: a scoop whose rim tops out at y 0.574, under `entry_y_left` 0.600, still stands in the orbit ray, because the ray crosses that height on its way past. At §0.8's binding 119.96° entry bearing the RF→left-mouth ray meets y 0.560 at x 0.08243 — **0.00217** from a scoop centred at 0.085, against the 0.0275 a 0.014 rim plus a ball needs. On the far side of the corridor the same ray clears the rim by **+0.03965**, and the scoop is still shot from RF, at 111.91° — **9.09°** inside the rest end rather than 1.28° — and clear of `legs_bank` (§3.4). Test every scoop near a lane against the *ray*, not against the mouth's y |
| `gantry_ramp` | ramp | path `[0.400,0.640]` → arc over top-right → `[0.112,0.300]` | `height_profile` `[{"s":0,"z":0},{"s":0.4,"z":0.052},{"s":1,"z":0.025}]`, `drop_exit` true | exits to left inlane |
| `flywheel_spinner` | spinner | `[0.029, 0.700]` | `facing_deg` **76** | in the left orbit lane, on §0.8 keep-out (c)'s spinner offset (0.0085 outboard of the 0.0375 lane center) so the plate's inboard end opens a 0.03387 m escape gap. Crossings: **0.73051 m/s** slowest descent, **1.13791 m/s** slowest climb, against the 0.31 m/s wall floor |
| `weld_targets` | standup_target ×2 | `[0.340,0.430]`, `[0.352,0.389]` | `facing_deg` 268 | weld points (modes light 5 virtual points across these + banks). **Sited east of the LF fan, and it must stay there (computed).** At `[0.240,0.410]`/`[0.280,0.395]` the pair sits inside every left-flipper line this table has: 0.00289 from the `torso_bank` ray, 0.00078 from the `gantry_ramp` ray and 0.00679 from the `crane_dock` ray, all against 0.026 — and standups never drop, so none of those would be shots. Nor is the pair's own centre gap a lane the `right_orbit` ray can thread: a ray 0.02103/0.02167 from the two centres looks clear, but the pair's **surface** gap is 0.042720 − 2·0.0125 = **0.01772**, under the 0.027 ball — always measure a pair's gap surface-to-surface. East of the fan they clear `torso_bank` by **+0.00876**, `gantry_ramp` by **+0.01310** and RF→`head_bank` by **+0.01161**, keep the same 0.04272 centre separation (surface gap 0.01772 ≤ 0.024, blocked and outside 09 §6's jam band), and are shot from **RF** at 89.82°/87.28° — whence `facing_deg` 268, the pair's own perpendicular to those two arrivals. From LF they bear 57.72°/52.40°, below its 59° rest end |

**Crane sequence (scripted, binding).** On `kicker_enter` at `crane_dock`
with a lock lit: hold 1200 ms (latch anim), then eject 1.2 m/s at 120°. The
ball leaves at `v = (−0.600, +1.039)` m/s and climbs under the 1.1105 m/s²
in-plane gravity, so it rises the 0.022 m from `[0.350, 0.786]` to the floor
plane in **21.4 ms**, crossing it at **x = 0.3372**. The floor's feed gap runs
x 0.310 → 0.372, so the ball (radius 0.0135) passes through it with
**0.0137 m** clear on the west edge and **0.0213 m** on the east: the gap is a
0.062 m break and the eject crosses it nowhere near either lip. Compute that
crossing x from the eject vector and the 0.022 m rise, never estimate it from
the picture. It then touches the roof 0.045 m up at ≈ 0.99 m/s
of climb, rebounds off wood (e = 0.30) and lands on the floor at x ≈ 0.30
rolling west at ≈ 0.55 m/s —
i.e. inside `crane_mag_a`'s field with almost no along-rail speed of its own.
Then the `tb.magnet_pulse` chain: `crane_mag_a` 260 ms at t=0, `_b` at
t=380 ms, `_c` at t=760 ms — each magnet drags the ball to its core and the
next one reaches that core (0.075 spacing < 0.090 radius), so the ball is in
`assembly_bay` and `ball_lock` fires by t ≈ 1.0 s.

**Stall failsafe (recomputed at these coordinates).** Gravity alone cannot
recover a stall: at the floor's 1.56° tilt the along-rail component of slope
gravity is 1.1105 · sin 1.56° = **0.030 m/s²**, an order below the
0.2437 m/s² rolling resistance of 08-physics.md §1.3, so the tilt only biases
a ball that is *already* rolling — it never restarts a dead one. What does is
magnet coverage: every point of the channel lies within 0.090 m of a crane
magnet, and a magnet at 0.05 m puts 1.6 N on the 0.08 kg ball (20 m/s², two
orders above rolling resistance). The script therefore **re-runs the whole
pulse chain every 2.5 s for as long as a crane lock is pending** (not once),
with no ball-state query needed; 11-game-framework.md §4.6 case A remains the
engine-side backstop. Light show: an orange claw insert tracks the pulsing
magnet.

### 3.4 Shot map

| Shot | From | Angle | Path |
|---|---|---|---|
| left_orbit | RF | **+28.6°** | spinner, around the top and out the right mouth (`orbit_merge_gate`, §0.8). `entry_y_left` 0.600 leaves only **1.04°** of mouth clearance (§0.8) — the narrowest band of the five (2.04° at 0.620, 3.88° at 0.660), and the rows below clear it end to end, so it is also the thinnest **usable** one. Mouth clearance alone never was the test (§0.8): neon-drift's nominal 2.04317° band was worth only **0.646°** until §1.3 moved `pit_scoop` out of it |
| right_orbit | LF | **-22.4°** | around top, exits left — **only with `torso_bank` down**: up, the row's west end sits 0.01355 off this ray (§3.3), 0.0125 short of the 0.026 it needs |
| head_bank | **LF** | **-10.0°** | straight up the middle. **RF (+6.7°) only with `torso_bank` down** — up, its west end grazes the RF ray at 0.02293 (§3.3) |
| torso_bank | LF | **-26.9°** | right-center bank face |
| legs_bank | **LF** | **+7.9°** | left bank face at the §3.3 coordinates — **not** RF: all three targets now lie past RF's 121° rest end |
| gantry_ramp | LF | **-26.3°** | ramp → left inlane — **only with `torso_bank` down**: the entrance is 0.67° behind that row from this flipper, and up it blocks the ray at 0.01143 (§3.3) |
| crane_dock | LF | **-17.3°** | scoop under the rail's feed gap at `[0.350, 0.786]` |
| control_booth | RF | **+21.9°** | left scoop at `[0.160, 0.560]` (§3.3), bearing 111.91° — **9.09°** inside RF's rest end |

**Clearance record (computed, binding; method and obstacle radii per §2.4,
error bars per §0.3).**

| Shot | Tested bearing | Tightest obstacle | ⊥ dist | Need | **Ball margin** |
|---|---|---|---|---|---|
| left_orbit | 119.96° (§0.8 floor) | `legs_bank` top `[0.135, 0.390]` | 0.03941 | 0.026 | **+0.01341** |
| left_orbit | 121° (RF rest end) | `legs_bank` top `[0.135, 0.390]` | 0.03323 | 0.026 | **+0.00723** |
| right_orbit (torso down) | 67.574° | `gantry_ramp` mouth `[0.400, 0.640]` | 0.03913 | 0.0355 | **+0.00363** |
| head_bank LF | 80.002° | `legs_bank` top `[0.135, 0.390]` | 0.05365 | 0.026 | **+0.02765** |
| head_bank RF (torso down) | 96.675° | `weld_targets` 1 `[0.340, 0.430]` | 0.03761 | 0.026 | **+0.01161** |
| torso_bank | 63.070° | `weld_targets` 1 `[0.340, 0.430]` | 0.03476 | 0.026 | **+0.00876** |
| legs_bank | 97.883° | nothing within 0.24 m of the line | 0.24659 | 0.026 | **+0.22059** |
| gantry_ramp (torso down) | 63.741° | `weld_targets` 1 `[0.340, 0.430]` | 0.03910 | 0.026 | **+0.01310** |
| crane_dock | 72.699° | `torso_bank` west end `[0.310, 0.560]` | 0.02902 | 0.026 | **+0.00302** |
| control_booth | 111.912° | `legs_bank` top `[0.135, 0.390]` | 0.08664 | 0.026 | **+0.06064** |

`crane_dock` at **3.02 mm** is the tightest row on any of the three tables and
`right_orbit` at 3.63 mm is next; both are at the floor, so neither
`torso_bank`'s west end nor the gantry mouth may move toward its ray. The
three drop-dependent rows are measured with the row **down** — with it up they
are −0.01245, −0.00307 and −0.01457, which is why they carry the qualifier
rather than a margin. Both millimetre rows are inside §0.3's indicative band:
they say "do not crowd this line", not "this shot lands", and
`shots.crane_dock.rate` / `shots.right_orbit.rate` are the verdict.

### 3.5 Rules

**Skill shot.** A light sweeps O-I-L at 2.5 Hz; soft plunge into the lane
that is lit at entry: 750,000 + spots one `head_bank` target.

**Robot assembly.** Completing a bank marks that part built (blueprint insert
fills; bank stays down). All three parts = "Robot Assembled": 500,000, all
banks reset, and one crane lock is lit at `crane_dock`. Each lock requires a
fresh full assembly. O-I-L lanes (lane-change) completed = +1 bonus X (max 5×).

**Full Assembly Multiball (4-ball).** Lock 3 balls via the crane sequence
(§3.3); with 3 locked, `crane_dock` starts 4-ball MB (all 4 trough balls;
virtual locks in multiplayer per 11-game-framework.md). Jackpots: any bank
completed = 600,000; all three = Super 3,000,000 at `crane_dock`; first
Super runs the crane as add-a-ball (one per MB). Light show: orange
full-field strobe on Super, crane claw victory sweep.

**Modes** — lit by O-I-L completion, started at `control_booth`, one each per
Overload cycle:

| Mode | Timer | Tasks | Awards | Light show |
|---|---|---|---|---|
| Weld Rush | 30 s | hit 5 lit weld points (weld_targets + lit bank targets) | 300,000 each, 1,500,000 completion | white arc-flash particles per hit |
| Quality Control | 40 s | knock the single lit target in head → torso → legs order, twice around | 400,000 each, 2,000,000 completion | lit target blinks teal, others dark |
| Conveyor Chaos | 45 s | conveyor on; lit shot moves every 5 s: left_orbit → gantry_ramp → torso_bank | 500,000 per catch (max 6) | belt chase lights under conveyor |

**Conveyor (scripted, binding).** While active: 600 ms cycle; `conv_1` pulse
150 ms at t=0, `conv_2` at t=200 ms, `conv_3` at t=400 ms — drags slow balls
rightward. Reversed order during Overload Phase 2. Active only during
Conveyor Chaos, Overload, and the first 10 s of Full Assembly.

**Combos ("Assembly Line").** Two different banks each hit within 3 s:
+250,000; three banks = +1,000,000 "Production Quota".

**Wizard — Overload.** Qualify: play all 3 modes + Full Assembly MB. Start at
`control_booth`. Phase 1 "Overcharge" (40 s): every drop target 250,000, banks
auto-reset 3 s after completion. Phase 2 "Meltdown" (3-ball): conveyor
reversed, all crane + conveyor magnets fire seeded random 200 ms pulses every
1.5–2.5 s (`tb.rng`), jackpots 1,000,000 on every bank. Payoff: 10th Meltdown
jackpot = "Factory Reset" 12,000,000, then all progress resets.

**Ball save:** 8 s. **Spare Part:** completing `legs_bank` twice in one ball
lights one outlane save.

**Extra balls:** (a) first Full Assembly MB; (b) 30 cumulative drop targets
in a game.

Scoring table (avg game score target 9,000,000):

| Award | Base points | PF-mult? | Avg count/game | % of avg game |
|---|---|---|---|---|
| Switch/rollover base | 5,000 | no | 200 | 11 |
| Spinner (per spin) | 6,000 | no | 45 | 3 |
| Drop target | 75,000 | no | 26 | 22 |
| Bank complete / Assembled | 200,000 / 500,000 | no | 4 / 1.1 | 15 |
| Mode shots | 300,000–500,000 | no | 4.0 | 17 |
| Mode completion | 1,500,000–2,000,000 | no | 0.4 | 8 |
| MB jackpots | 600,000 | no | 1.5 | 10 |
| Super jackpot | 3,000,000 | no | 0.15 | 5 |
| Combos | 250,000–1,000,000 | no | 1.0 | 3 |
| End-of-ball bonus | 15,000 ×targets ×bonusX | no | 3 balls | 6 |
| Overload payoff | 12,000,000 | no | 0.02 | 3 |
| Skill shot | 750,000 | no | 1.1 | 9 |

No playfield multiplier on this table; bonus X (O-I-L) is the multiplier
economy. `meta.replay_score` **14,000,000** (§0.6: 1.56 × the 9.0M
`score.p50` band midpoint). High score defaults (10 entries, §0.6; rank 1 =
5 × 9.0M, rank 10 = 9.0M): 1) 45,000,000 BOT ·
2) 36,000,000 COG · 3) 28,000,000 TIN · 4) 22,000,000 ROM · 5) 17,000,000 OIL
· 6) 15,000,000 NUT · 7) 13,000,000 ARM · 8) 12,000,000 RIV · 9) 10,500,000
WLD · 10) 9,000,000 CAM.

### 3.6 Sound / music brief

| Key (§0.9) | Where | Patch | Character |
|---|---|---|---|
| `drop_target` | map | `tt_press` | hydraulic press thunk + metal ring, 300 ms |
| `ball_lock` | map | `tt_clamp` | heavy clamp + relay click |
| `magnet` | map | `tt_crane` | servo whine (the engine retriggers it every 500 ms while held) |
| `tilt_warning` | map | `tt_alarm` | single factory klaxon whoop |
| bank complete | `tb.play_sound` | `tt_partdone` | 3-hit rivet burst + steam hiss |
| crane pulse a/b/c | `tb.play_sound` | `tt_crane_a/b/c` | servo whine, pitch tracks a→b→c (600→450→350 Hz) |
| conveyor tick | `tb.play_sound` | `tt_belt` | low rubber thud each pulse, 80 ms |
| jackpot | `tb.play_sound` | `tt_stamp` | industrial stamp + choir-saw stab |

Music (12-audio.md §9 reserved song ids): `attract` 100 BPM music-box
automation theme; `main` 112 BPM industrial stomp (anvil percussion, square
bass ostinato); `mode` 120 BPM adds ratchet 16ths; `multiball` 138 BPM full
machine-room wall; `wizard` 144 BPM with detuned sirens and half-time
breakdown on Phase 2 entry; `game_over` 3-bar power-down of the stomp loop,
last hit a steam release.

### 3.7 Difficulty targets

`ball_time_s.p50` 28–40 s · `score.p50` 6.5M–11.5M · `score.p90 ÷ score.p10`
≤ 5.0 · `drains.center` ≤ 0.34 (a real narrowing of 14 §8.3's "< 0.35" — bank
rejects come down the middle, so verify posts at `head_bank` deflect rejects
off-center) · `shots[crane_dock].rate`
0.14–0.24 · `shots[gantry_ramp].rate` 0.18–0.30 · `modes.started_per_game`
1.6–4.0 · `modes.multiball_reach_share` 0.15–0.30 ·
`modes.wizard_reach_share` 0.01–0.04 · `stuck_balls` 0 (crane failsafe §3.3
must fire in the stall test).

### 3.8 Build notes

Built complete in **M17** (first of the three M17 tables; if M17 is split,
`M17a`). Requires M7 drop banks/locks, M8 magnets and ramp, M15 tooling.
Crane and conveyor scripts are pure `rules.lua` over `tb.magnet_pulse` — no
engine feature may be added for them.

### 3.9 Distinctiveness

Tilt-O-Tron is the methodical table: no pops, no playfield multiplier, almost
no randomness outside Meltdown — every point is deliberately aimed
drop-target work and the long lock grind to a 4-ball payoff. It has the
game's only physical ball transport (the crane) and the highest score per
shot, rewarding patience where Neon Drift rewards speed.

---

## 4. cosmic-carnival — Cosmic Carnival

### 4.1 Fantasy

A big-top circus pitched on an asteroid. Every launch is a cannonball act —
you aim the cannon yourself against a sweeping ring of lights — and the
midfield is all motion: three spinners like spinning plates, clown-horn
bumpers, and a juggling multiball tossing jackpots side to side. The tiny
flipper on the right wall makes the tent shot the trickiest act on the bill.

### 4.2 Art

- **Palette:** `vapor-pink` (13-art-direction.md §2.2, binding assignment):
  deep violet star-field grounds (bg0 `#1A0B2E`), hot pink primary
  (`#FF71CE`), electric cyan secondary (`#01CDFE`), neon mint / violet
  accents (`#05FFA1` / `#B967FF`), amber `warm` flashes, `glow_white`
  spotlights.
- **Motifs:** (1) striped big-top canopies (pink primary alternating bg1)
  over lanes and scoops; (2) ringed planets as juggling balls (cyan/violet)
  arcing across the field; (3) marquee bulb borders (`glow_white` dot-light
  strings) around every insert group.
- **Logo:** "COSMIC CARNIVAL" on a curved marquee arch, hot-pink serif
  circus letters with `glow_white` bulb dots, a mint comet (accent1)
  swinging through like a trapeze.

### 4.3 Layout

```
+--------------------------------------+
|         _____________                |  the right orbit leg carries nothing
|        _B___I___G_       (K)         |  K: cannon breech, just under the
|       /(-o-)(-o-)(-o-)\   /|         |     right orbit mouth (aims/fires)
|    */   ____       T     |;|         |  T: ringmaster standup · * plate_r
|    |   |TENT|            |;|         |  TENT: scoop + 2-ball lock, in the
|    |   |(K)L|    O       |;|         |     alcove over tent_wall (\__/),
|    |*   \__/    O O      |;|         |     which blocks LF+RF, not MF/cannon
|    |;|        *          |;|         |  O: clown pops · *: plate_l / plate_r
|    |;|  %L    |      %R  |;|         |  center: hoop spinner, feeds the pops
|    |;|   \    |     /     |;|        |  %L %R ramp entrances
|    |;|    \   |    /    T |;|        |  T: J-U-G standups (right)
|    |;|     |  |   |     T |;|        |
|    |;|     |  |   |     f-|;|        |  f: cannonade mini flipper
|    | /S\   |  |   |  /S\  |;|        |     (right wall, aims at TENT)
|    | \_/ __|  |   |__\_/  |;|        |
|    |-o-\ /         \ /-o- |P|        |
|    | \  \   F   F   /  /| |'|        |
|     \ '  \ (L) (R) /  ' /           |
|      \____   DD   ______/           |
+--------------------------------------+
```

Prefabs: standard bottom (§0.4) **except `plunger_lane`**, which this table
hand-places (see the shooter-lane rows below); `orbit` per **§0.8**
(`mouth_x` 0.075, `top_radius` 0.130, `entry_y_left`
**0.620** — **0.030 above §0.8's 0.590 floor** and worth
**2.04317°** of mouth margin against RF's 121° rest end, and still under `tent_wall`'s left node at
`[0.075, 0.630]`, which stays *on* the guide — `entry_y_right` 0.900) with the §0.8 merge (`orbit_merge_gate`); `top_lanes_n`
n=3 labels B-I-G `pos` `[0.240, 0.880]`, `lane_length` 0.050 (lane centers x
0.200 / 0.240 / 0.280); `pop_cluster` n=3 centroid `[0.290, 0.700]` spacing
0.072 — moved down out of the §0.8 top band **and** clear of every cannon ray
(§4.3 Cannon: the tightest is the 204° tent ray, which passes 0.05265 from
`pop_1`'s centre, a **+0.00815 m** ball margin — 0.0127 of which is bought by
the last 0.014 m of that drop, so the centroid may not rise); `ramp_standard` ×2 (left ramp exits right inlane, right ramp
exits left inlane — mirrored returns).

Element roster:

| id | type | pos | key params | notes |
|---|---|---|---|---|
| `cannon_breech` | kicker | `[0.4625, 0.876]` | `style` "scoop", `capture_ms` 400, `eject_speed` 4.0, `eject_angle_deg` 90 | **in the right mouth's throat, 0.018 below the aperture** `[0.445, 0.900]`–`[0.480, 0.888]` (§0.8) and in open playfield: the guide wall does not start until y 0.900 and the shooter wall tops out at 0.888, so the breech still has line of sight across the whole field. x = 0.4625 is the mouth's mid-line and the only x from which a 90° eject threads the 0.037 m mouth — **0.0040 m** clear of the shooter wall's top node and the same of the guide wall's lower node, with nothing in between (the merged lane emits no top post, §0.8 — a post there would stand **0.015305** from this centre against the 0.0215 a post + ball needs, putting the breech inside it with no eject able to resolve); the scoop's own 0.014 m rim stands 0.0035 m off `shooter_wall_high`, blocked (≤ 0.024) and clear of the jam band. It captures every ball that uses the right mouth **in either direction**: a LOAD shot from RF going in (§4.4 — the LF line is blocked), and the ball a soft plunge sends back out (§0.8) — which is what makes the skill shot a cannon shot. A **full plunge does not pass here**: it merges into the orbit above the mouth and rides over the top, so the first-launch cannon load is won with the soft plunge, not the hard one. With LOAD unlit the script immediately kicks it on at the listed default (4.0 m/s at 90°, up the empty leg and around the top), so the right orbit still plays as a loop. Not a `vuk`: a vuk must bind to a ramp end within 0.03 m and ignores eject angles (08-physics.md §6.9, 09-table-format.md §4.12), and no ramp end is here; every aimed shot kicks with an explicit angle. **Verification against a post-free mouth (computed) — re-run all three checks if any node here moves.** *Held ball clears every surface:* centred at `[0.4625, 0.876]` the ball has **+0.0040** to `shooter_wall_high`'s face (x 0.480), **+0.00772** to its top node `[0.480, 0.888]` — which is also the coincident inner end of `orbit_merge_gate` and `shooter_gate`, centre distance `sqrt(0.0175² + 0.012²)` = 0.021219 − 0.0135 — and **+0.016203** to the guide wall's lower node `[0.445, 0.900]` (0.029703 − 0.0135); it sits 0.017027 perpendicular under the mouth line, i.e. 0.003527 below the aperture plane, and nothing else is inside 0.030. *The 90° eject resolves:* straight up the mouth's mid-line it passes both aperture nodes with **0.0040** to spare and meets no solid collider before the top corner, because §0.8's keep-out (a) bars one, keep-out (b) empties the leg of spinners, and the right entry switch is an `open` gate. *The soft-plunge load reaches the disc:* a ball turned out of §0.8's 72° fan descends hugging the wall on x 0.4665, so it passes the breech centre at **0.0040** — inside the 0.014 m capture disc (08 §6.9) with 0.010 to spare — and cannot slip between scoop and wall, that gap being 0.0035 against a 0.027 ball; a ball that drops straight through the 0.037 m mouth instead has its centre confined to x ∈ [0.4585, 0.4665], again **≤ 0.0040** off the disc centre. Capture is geometric, not lucky |
| `aim_lights` | light ×5 | arc r 0.06 around breech | `shape` "arrow", `direction_deg` per target | sweep 4 Hz through the five **computed, non-uniform** bearings of §4.3 Cannon: 195° `ringmaster` / 204° `tent_scoop` / 226° pop nest / 252° `right_ramp` / 266° `jug_targets` |
| `ringmaster` | standup_target | `[0.185, 0.804]` | `facing_deg` 21 | the "ringmaster" board, faces the breech; the cannon's flattest lit angle and a lucky RF ricochet target. y 0.804 (not 0.795) is what buys the 195° ray its **+0.00827 m** margin under the B-I-G divider end while keeping 9° to the 204° tent ray. Its face ends 0.0393 from that divider end — passable (≥ 0.033), not jam band |
| `tent_scoop` | kicker | `[0.100, 0.712]` | `style` "scoop", `eject_angle_deg` -50, `eject_speed` 3.0 | mode start, inside the tent: the alcove bounded by the left orbit guide (x 0.075), `tent_wall` below and open field to the east. Its 0.014 m rim stands **0.011 m** off the guide wall — blocked (≤ 0.024), clear of the jam band. Reachable only by the cannon's 204° ray and by `cannonade_flipper` (bearing 135.16°); both main flippers are walled off (`tent_wall`). The −50° eject leaves the alcove the same way a ball enters it, clearing the wall's east node by **0.0112** ball margin |
| `tent_wall` | wall | path `[0.075, 0.630]` → `[0.155, 0.608]` | `material` "plastic" | the tent's canvas front, length 0.0830, sloping **15.4° down to the right** — above 08 §1.3's 12.7°, so a ball that lands in the alcove and misses the scoop always rolls off the east end instead of resting on it. Left node is *on* the orbit guide wall (gap 0.000); the east node clears the left ramp mouth's upper flange `[0.1656, 0.5755]` by **0.0342** (passable, ≥ 0.033) and `pop_2`'s cap by 0.0910. See §4.3 Tent wall for the blocking arithmetic |
| `tent_lock` | ball_lock | `[0.100, 0.758]` | `capacity` 2, `style` "hidden" | behind (above) the tent scoop, 0.046 up-table of it; locked balls vanish into the tent. The 204° cannon ray — the rounded angle the cannon actually fires — passes **0.03964** from it, **+0.01964** against the **0.020 m** capture radius (08 §6.14 tests a circle of radius 0.02 m on the ball **centre**, so no ball radius comes off; §2.4's radius list carries the row now) [0.04191 ⇒ **+0.02191** at the exact 204.343° bearing]. Do not borrow the neighbouring `tent_scoop`'s 0.014 m kicker-rim figure for it — that reads +0.01214 and is the wrong radius for a lock. On the **195°** ringmaster ray the lock sits at 0.38122 — **0.09452 beyond** the `ringmaster` face that ray is aimed at — and clears the same circle by only **+0.00016** rounded [+0.00318 exact], so a shot that misses the standup and runs on is captured rather than lost; the unlit-lock `tb.release_lock` pattern (10-scripting.md) is what returns it |
| `plate_l` | spinner | `[0.029, 0.680]` | `facing_deg` **76** | in the left orbit lane, on §0.8 keep-out (c)'s spinner offset (0.0085 outboard of the 0.0375 lane center) so the plate's inboard end opens a 0.03387 m escape gap. Crossings: **0.68117 m/s** slowest descent (0.50622 at `plate_r` above it, less that plate's 0.11644 axial loss, then 0.180 m of fall), **1.25355 m/s** slowest climb, against the 0.31 m/s wall floor |
| `plate_r` | spinner | `[0.029, 0.860]` | `facing_deg` **76** | in the **left** orbit lane on §0.8 keep-out (c)'s spinner offset, 0.180 up-lane of `plate_l`; still spun by every ball the breech kicks on, because a 4.0 m/s eject is far past §0.8's 1.6906 full-loop threshold, so every kicked ball rides the top and comes back down this leg. **Why no plate may sit in the right leg (computed).** Put one at `[0.4825, 0.905]`, `facing_deg` 90 and it is a level plate whose wall normal is exactly 90°, the one normal that carries gravity. 08 §6.6 makes a spinner "not a solid collider **(except when too slow to pass)**": under `s_pass` 0.15 m/s the plate is a `steel` wall for that contact. A plunge reaching y 0.905 at 0.15 m/s is `v0` **1.54672**; passing costs 0.12 m/s, so the ball re-crosses downward at `v − 0.12`, under 0.15 for `v0` < **1.56293** — every plunge in **[1.54672, 1.56293)** comes to rest **on** the plate, and that band sits inside the 1.524–1.6028 mouth-feed window this table's skill shot is authored on. Tilting it does not help (a plate spans its lane, Common pitfalls, so a tilted one wedges against a lane wall), and the whole straight leg lies inside §0.8's merge corridor — which is why §0.8's keep-out (b) bars spinners from the merged right lane outright. **"Off the plunge path" is not what makes the left leg safe.** A ball descending it after the loop crosses y 0.860 at **0.50622 m/s** — `sqrt(2[1.11052·0.1665 − 0.24367·0.233])`, the path being the quarter arc **0.18300** plus the 0.050 straight run (split the arc on the ball *centre*'s tangent point, §0.8, or the path reads 0.193882 and the speed comes out wrong) — far above the wall band. But §4.4's own `left_orbit` row sends an RF ball **up** this leg past both plates, and a climbing ball can be arbitrarily slow, which is exactly the stall keep-out (b) describes, mirrored. §0.8 keep-out **(c)** is what closes it: the 0.0085 outboard offset and `facing_deg` 76 give the plate a 14.0° lean and a 0.03387 m inboard escape gap, so nothing rests on it. Slowest climb here is **0.89750 m/s** (0.78107 to finish the loop from y 0.860, plus the 0.11644 axial loss), **+0.58750** over the 0.31 m/s floor |
| `hoop` | spinner | `[0.290, 0.600]` | `facing_deg` **76** | center lane feeding the pop nest **0.0482** above it: nest bottom caps y 0.6482 (`pop_2`/`pop_3` y 0.679215 − the 0.031 cap) against the plate centre y 0.600, and **0.04518** from the plate's own high end (half-span 0.0125 along its **14.0°** span ⇒ y 0.603024). Passable on either measure (≥ 0.033). **`facing_deg` 76, not a near-level 92 (§0.8 keep-out (c)):** at 92 the plate leans 2°, under the 12.675° at which gravity restarts a stopped ball, so a ball that settles on it stays. This is the one spinner here that is *not* in a lane, and therefore the one the 14.0° lean alone secures: both plate ends open on field, so a restarted ball rolls straight off the low end |
| `left_ramp` | ramp | `[0.150,0.560]` → cross right → `[0.408,0.300]` | profile as neon-drift left_ramp | "trapeze L" |
| `right_ramp` | ramp | `[0.352,0.545]` → cross left → `[0.112,0.300]` | mirrored profile | "trapeze R" |
| `jug_targets` | standup_target ×3 | `[0.442,0.560]`, `[0.452,0.528]`, `[0.459,0.496]` | `facing_deg` 200 | J-U-G |
| `cannonade_flipper` | flipper | `[0.462, 0.352]` | `length` 0.055, `rest_angle_deg` **−132**, `swing_deg` 42, `side` "right", `input` "right", `strength` 0.85 | wall-mounted (pivot 0.018 off `shooter_wall_high`, 0.007 clear of its `radius_base` 0.011); shoots up-left at the tent. **Launch window (computed, binding).** 09 §4.3 sweeps a `side` "right" flipper from `rest_angle_deg` to `rest_angle_deg − swing_deg` — here **−132° → −174°** (228° → 186°) — and the bat launches perpendicular to itself in the sweep direction, so the ball leaves on **138° → 96°**. The tent line from this pivot is **135.16°** (§4.3 Tent wall), **2.84°** inside the rest end; `tent_scoop`'s 0.014 m capture disc at 0.5105 m subtends **±1.571°**, so the entire capture cone **133.59°–136.73°** falls inside the window, with **1.27°** to spare at the top and 37.59° at the bottom. **The sign matters here too:** a positive `rest_angle_deg` (128) sweeps 128° → 86° for bearings **38° → −4°** — up-*right*, into `shooter_wall_high` 0.018 m away — leaving the tent unreachable by any flipper and §4.7's 14 §8.3 exception with no feeder. The same convention reproduces every other flipper here: standard RF (−149, 52) ⇒ 121°→69°, bracketing all of §4.4's RF shots; standard LF (−31, 52) ⇒ 59°→111°, bracketing its LF shots. At rest the bat spans `[0.4252, 0.3111]`–`[0.462, 0.352]`, under and left of `feed_gate`'s drop, and the sweep lifts that span up-left through the drop zone; the tip is below the pivot at every angle from −132° to −174°, so the **whole swept envelope tops out at y 0.363** (`pos.y` 0.352 + `radius_base` 0.011) |
| `shooter_wall_low` | wall | path `[0.480,0.000]` → `[0.480,0.412]` | `material` "wood" | lower half of the hand-placed shooter lane; starts at y 0 so the lane is watertight against the bottom wall (09 §5.2) |
| `shooter_wall_high` | wall | path `[0.480,0.448]` → `[0.480,0.888]` | `material` "wood" | upper half, topping out **on** 09 §5.2's gate line `y_gate = top_y + 0.008 = 0.888` — which is also the inner endpoint of §0.8's `orbit_merge_gate` and the outer edge of the right orbit mouth. The 0.036 m break between the two walls is `feed_gate`'s aperture |
| *(no top post)* | — | — | — | this lane merges into the orbit, so it takes 09 §5.2's **merged variant**: three elements, no `<id>_top_post`. A post at `[0.472, 0.888]` would sit 0.002595 from the mouth line and leave 0.021547 to the guide's lower node (§0.8) — the right mouth would pass nothing, `cannon_breech` at `[0.4625, 0.876]` would be **0.015305** from the post centre where 0.0215 is needed (i.e. geometrically inside it, no eject resolvable), and the post + closed flap would trap balls at `[0.486145, 0.904192]`. `shooter_wall_high`'s top node is a rounded end cap on its own |
| `shooter_gate` | gate | `[0.500, 0.888]` | `facing_deg` 90, `width` 0.040 | one-way lane exit, the prefab's `<id>_gate` verbatim: `[W − lane_width/2, y_gate]`. Its span `[0.480,0.888]` → `[0.520,0.888]` puts the inner endpoint on `shooter_wall_high`'s top node — the merged variant emits no top post, so that node is the only surface there — and the outer endpoint on the boundary wall — **0.000 m at both ends**, against V014's 0.006 m tolerance |
| `shooter_plunger` | plunger | `[0.500, 0.030]` | `max_speed` 7.5, `charge_time_s` 1.5 | as the prefab's `<id>_plunger` |
| `feed_gate` | gate | `[0.480, 0.430]` | `facing_deg` **180**, `width` **0.036**, `default_state` "one_way" | one-way from the shooter lane into the playfield, and the one element that is **not** in the prefab expansion. Its span is perpendicular to `facing_deg`, i.e. `[0.480,0.412]` → `[0.480,0.448]`, so both endpoints land **exactly** on the two wall ends (V014 wants ≤ 0.006), and V014's 0.030 m test ray at 180° (from `[0.480, 0.430]` to `[0.450, 0.430]`) runs into open field: `cannonade_flipper`'s swept envelope tops out at **y 0.363** (roster row above), leaving the ray **0.067 m** of clear air. The aperture is **0.036 m**, not 0.030: balls pass through it, so it must clear 09 §6's 0.025–0.032 jam band *and* 14 §4.3's "open it above 34 mm" trap rule — the same 0.036 the tilt-o-tron crane channel uses (§3.3). It is only ever passable because of `feed_deflector`: gravity in the lane is pure −y, so a ball falling back down it has `v = (0, −v)` and `dot(v, f̂) = 0` — a one-way gate is a solid wall at `dot ≤ 0` (08 §6.7), and without the deflector the ball would roll straight past to the plunger and this gate would never open for anything |
| `feed_deflector` | gate | `[0.500, 0.41928]` | `width` **0.04257**, `facing_deg` **110**, `default_state` "one_way" | the element that gives the returning ball its −x. The span is perpendicular to `facing_deg` (08 §6.7), so `width` = `0.040/cos 20°` = 0.042567 makes the flap run `[0.480, 0.412]` → `[0.520, 0.42656]`: exactly **20° above +x**, right across the 0.040 m lane. Inner node *is* `shooter_wall_low`'s top node and `feed_gate`'s lower span endpoint (V014 **0.000**), outer node is on the boundary (**0.000**). **Up:** a plunged ball has `v = (0, +v)`, `dot(v, f̂(110°)) = cos 20° = +0.940 > 0` → no collider at all, so §0.8's plunge thresholds and 1.06300 m path are untouched. **Down:** `dot = −0.940 ≤ 0` → wall (steel, restitution forced to 0.3, 08 §6.7), and the ball slides the 20° face — above 08 §1.3's 12.7° — down to the inner node, arriving at `feed_gate` on bearing 200° with `dot(v, 180°) = +0.940 > 0`, which is what opens it. Its centre crosses x 0.480 at y **0.42637** (`0.412 + 0.0135/cos 20°`), inside the aperture, clearing `shooter_wall_high`'s bottom node by **+0.00683** (`0.036·cos 20° − 0.027`); the lower lip is the flap's own end node, so that transition is continuous, not a gap. V014's 0.030 m ray from `pos` along 110° ends at `[0.4897, 0.4475]`, **0.0097** short of the lane wall, hitting nothing. Both ends land on colliders, so the flap adds no gap of any size — nothing to fall in the 0.025–0.032 band |

The four hand-placed shooter-lane elements **are** the documented
`plunger_lane` expansion in its **merged variant** (09-table-format.md §5.2
with `lane_width` 0.040, `top_y` 0.880, `max_speed` 7.5 — three children, no
top post, the wall authored here in two halves), copied into `elements` and the prefab instance deleted,
because `feed_gate` needs a hole in the lane wall and 09 §5 forbids relying
on edits to generated elements. Ids differ from the generated
`<instance>_wall`… names on purpose: nothing may look like a prefab child.

Every coordinate above is that expansion's arithmetic, not a hand guess.
With `W = 0.520`: `xw = W − lane_width = 0.480` and
`y_gate = top_y + 0.008 = 0.888`, giving `<id>_wall` `[0.480,0.000]` →
`[0.480,0.888]` (authored here as the two `shooter_wall_*` halves),
`<id>_gate`
`[W − lane_width/2, y_gate]` = `[0.500,0.888]` width 0.040, and
`<id>_plunger` `[0.500,0.030]`; `<id>_top_post` is not emitted, because the
mouth node `[0.445, 0.900]` is 0.037 from the wall's top node — inside §5.2's
0.045 merge test. The **only** licensed deviations are the
split and the added `feed_gate` + `feed_deflector` pair. Move any of those numbers and three things
break at once: the gate span leaves V014's 0.006 m tolerance (so the table
cannot meet the zero-warnings Done-when), the right mouth stops being 0.037 m
wide, and §0.8's `orbit_merge_gate` no longer ends on the
wall's top node.

**Cannon (scripted, binding).** A **mouth-feed plunge** (§0.8's stub or soft
band, 1.524–1.6028 m/s) stalls on the flap or in the merged right leg, is turned
back out of the right mouth by
`orbit_merge_gate`, and drops into `cannon_breech` — that is the ball's
cannon load. A **full plunge** (≥ 1.6906) rides the orbit over the top and out the left
mouth instead, forfeiting the skill shot. A **dead plunge** (< 1.524) splits
in two, and the split point is computed, not asserted: the ball must lift its
centre to y **0.43365** to touch `feed_deflector`'s face at the lane centre
(flap y 0.41928 there, plus 0.0135/cos 20°), i.e. `Δh` 0.40365 at the §0.8
deceleration 1.3542 m/s² ⇒ **v ≥ 1.0456 m/s (13.9 % charge)**. Below that it
never reaches the flap and rolls back onto the plunger for a re-plunge — the
only part of the dead band that does, which is the exception §0.8's dead row
carves out for this table. Between **1.046 and 1.524 m/s** it falls onto
the flap, slides down it at 200°, and that −x is what lets it through
`feed_gate` onto `cannonade_flipper` — the player loses the cannon shot and
gets a mini-flipper shot at the tent instead. Both halves of the dead band are
reachable off the plunger and neither loses the ball. On
`kicker_enter` with LOAD lit the script calls
`tb.kick_hold("cannon_breech")` — the ball becomes script-held indefinitely
and the `capture_ms` auto-eject is canceled (10-scripting.md §3.4) — and the
five `aim_lights` sweep at 4 Hz; while the breech holds a ball, either
flipper-button `switch_hit` (`button_flipper_left`/`_right`,
10-scripting.md §4.1) fires
`tb.kick("cannon_breech", 5.0, angle_of_lit_light)` — button presses aim the
cannon instead of lane-change for the duration of the hold. With LOAD unlit
the handler instead kicks immediately at 4.0 m/s / 90° so the ball carries on
around the orbit. **Script-side timeout (binding):** `tb.kick_hold` opts out
of the sim's `capture_ms` auto-eject (08-physics.md §6.9), so the same
handler arms a `tb.timer(5000, …)` that fires at the lit light if no button
arrives — the script owns every release path. Mid-game, right_orbit while
"LOAD CANNON" is lit feeds the breech directly.

**The five lit angles (computed, binding).** Bearings from the breech at
`[0.4625, 0.876]` to each target centre, degrees CCW from +x (§0.1). **One
method for every row:** the ray is the *ball centre's* path; for each obstacle
take the **perpendicular** distance from that ray to the obstacle's centre (or
to a wall/divider segment), then subtract the obstacle's own radius **and** the
0.0135 ball radius. What is left is the **ball margin** — the width a ball has
to spare beside the obstacle, positive = passes, negative = struck. No row
quotes a vertical drop, a centre-to-centre distance, or a clearance that has
not had the ball radius taken out of it. Margins are given at the **rounded
lit angle the cannon actually fires**, with the exact bearing in brackets; the
floor for every row is **0.006 m**:

| Lit angle | Exact bearing | Target (centre) | Distance | Tightest obstacle | ⊥ dist | **Ball margin** |
|---|---|---|---|---|---|---|
| **195°** | 194.545° | `ringmaster` `[0.185, 0.804]` | 0.2867 | B-I-G divider end `[0.300, 0.855]` (segment, r 0) | 0.02177 | **+0.00827** [+0.00698] |
| **204°** | 204.343° | `tent_scoop` `[0.100, 0.712]` | 0.3979 | `pop_1` `[0.290, 0.741569]` cap r 0.031 | 0.05265 | **+0.00815** [+0.00688] |
| **226°** | 225.575° | pop nest `[0.290, 0.700]` | 0.2464 | divider end `[0.300, 0.855]` — the nest **is** the target | 0.10230 | **+0.08880** [+0.08785] |
| **252°** | 251.539° | `right_ramp` entry `[0.352, 0.545]` | 0.3490 | `pop_3` `[0.326, 0.679215]` cap r 0.031 | 0.06901 | **+0.02451** [+0.02266] |
| **266°** | 266.288° | `jug_targets` top `[0.442, 0.560]` | 0.3167 | `right_ramp` mouth `[0.352, 0.545]`, half-width 0.022 | 0.08714 | **+0.05164** [+0.05334] |

The **204° tent row is the tightest** at **8.15 mm** (6.88 mm at the exact
bearing), with the 195° ringmaster row next at 8.27 mm (6.98 mm exact). The
252° row only *looks* tightest if its figure is read without the ball radius
taken out — every row in this table has it taken out, and any re-run must too.
Both tight rows clear the 0.006 m floor at the rounded angle and at the exact
bearing, and both sit inside §0.3's indicative band.
The arithmetic behind them: `pop_cluster` centroid `[0.290, 0.700]`,
`spacing` 0.072, so vertices sit `d = 0.072/√3 = 0.041569` out (09 §5.9) —
`pop_1` (90°) `[0.290, 0.741569]`, `pop_2` (210°) `[0.254, 0.679215]`,
`pop_3` (330°) `[0.326, 0.679215]`; and `top_lanes_n` `[0.240, 0.880]`
`lane_length` 0.050 puts four divider **lower** ends on y 0.855 at x 0.180 /
0.220 / 0.260 / 0.300 (§0.8), the x 0.300 one being the corner every flat
leftward ray has to duck. Ray-local geometry at the breech itself — the
0.0040 m to the shooter wall's top node and the guide wall's lower node, the
rim's 0.0035 m to `shooter_wall_high` — is the mouth-throat arithmetic in the
`cannon_breech` row above and is not re-measured here. One obstacle sits
**past** its row's target and so is not the row's tightest: `tent_lock`'s
0.020 m capture circle (§2.4) is 0.09452 beyond `ringmaster` on the 195° ray
and clears it by **+0.00016** [+0.00318 exact], which only bites for a shot
that misses the standup outright — a capture, not a loss (`tent_lock` row).

The two tight rows are a squeeze, so both ends of it are pinned: `ringmaster`
at y 0.804 rather than 0.795 flattens its ray until it just clears the divider
end, and `tent_scoop` at `[0.100, 0.712]` rather than `[0.170, 0.740]` puts
the tent ray in the 0.0829-wide channel between `pop_1`'s cap and that same
divider end. Raise the tent and the 204° ray closes on 195°; drop it and
`pop_1` eats the 1,500,000 tent skill shot and the Human Cannonball act.

Adjacent lit angles differ by **9°, 22°, 26° and 14°** — all above the 8°
distinguishability floor, and deliberately non-uniform because the targets
are. Rounding to whole degrees costs at most 2.8 mm of aim error at the target
(252°), inside every target's own capture width. **Never author these five as
an even fan**: a uniform set such as 200/222/244/266/288 fired from a breech
inside the 0.040 m shooter lane bears on nothing — every leftward eject meets
the lane wall 0.025 m away, and the two flattest point at empty playfield.
Each angle is `bearing(breech → that target)`, recomputed whenever the breech
or a target moves.

**Tent wall (computed, binding).** `tent_wall` `[0.075, 0.630]` →
`[0.155, 0.608]` is what makes the tent a mini-flipper shot instead of a
main-flipper one, and the claim is a geometric one, checked against the
scoop's real **0.014 m capture disc** (08 §6.9 — a scoop captures on centre
proximity, so mouth facing proves nothing):

| Line (pivot → `tent_scoop`) | Bearing | Length | Result |
|---|---|---|---|
| LF `[0.141, 0.115]` | 93.93° | 0.5984 | **blocked** — crosses the wall at `[0.1062, 0.6214]` |
| RF `[0.339, 0.115]` | 111.82° | 0.6431 | **blocked** — crosses at `[0.1400, 0.6121]` (and the left ramp mouth is in the way first) |
| `cannonade_flipper` `[0.462, 0.352]` | 135.16° | 0.5105 | **open** — passes the wall's east node with **+0.02146** ball margin, and 135.16° is inside the flipper's computed 138°→96° launch window (roster row above) |
| cannon, 204° | — | 0.3979 | **open** — **0.05362** ball margin off the wall |

Blocking is checked over the whole cone into the capture disc, not just the
centre line: from LF the wall spans bearings 88.37–97.30° against a capture
cone of 92.59–95.27°, from RF 110.47–117.14° against 110.57–113.07°. The
narrowest escape is RF's lower tangent, which passes **0.0009 m** from the
wall's east node — a ball would need 0.0135 m there, so it is short by 0.0126.
The mini flipper's corridor is genuinely open on both sides: the line runs
0.0546 perpendicular from `hoop`'s centre above it (0.0161 of ball margin even
if that plate spans a full 0.050 m lane) and 0.0350 from the wall's east node
below it (0.0215), a **0.0646 m** free channel for a 0.027 m ball, with
`pop_2` a further 0.0409 clear and the right ramp mouth 0.0238. Gaps at the
wall's ends are 0.000 (its west node is *on* the
orbit guide), **0.03418** to the left ramp's upper flange `[0.1656, 0.5755]`
and 0.0910 to `pop_2`'s
cap — none of them in 09 §6's 0.025–0.032 jam band — and the 15.4° slope
means the alcove drains east rather than holding a ball.

**Which measure the 204° row uses.** The cannon row is measured exactly like
the five lit-angle rows above: perpendicular from the **unbounded** 204° ray
to the wall segment, minus the 0.0135 ball radius. The controlling point is
the wall's **west** node `[0.075, 0.630]` at ⊥ 0.06712 ⇒ **0.05362**
(0.05092 at the exact 204.343° bearing). **Do not clamp the path at the scoop
centre** — the perpendicular foot falls 0.056 m *past* `tent_scoop`, so a
clamped measure reads a slacker 0.07219. The unbounded ray is what binds: a
ball that misses the 0.014 m disc keeps going.

**The 0.03418 flange gap: declared, not widened.** It clears the band's
0.032 edge by only 0.00218, less than one §0.7 step-5 post move (±0.004),
where neon-drift §1.3 buys 0.004918 / 0.005860 for its two gaps. The
asymmetry is deliberate, and the reason is that step 5 moves **shot-mouth
posts**: neither side of this gap is one, and none of §0.7's other four
steps (outlane gap, ball save, `kick_speed`, `slope`) touches it either.
`tent_wall`'s east node is pinned four ways at once — the RF blocking line
crosses the wall just **0.0156** along it at `[0.1400, 0.6121]`, the 135.16°
mini-flipper line clears the node by 0.0215, `tent_scoop`'s −50° eject by
0.0112, and the 15.4° drain slope fixes its y once the west node sits on the
orbit guide at x 0.075 — and the flange is not an authored point at all: it
is `left_ramp`'s own 0.022 mouth half-width off `[0.150, 0.560]`. The
failure directions differ too: neon-drift's controlling gap must stay
**blocked** (below 0.025), this one must stay **passable** (above 0.032)
because it is the alcove's only drain. **Binding:** both nodes are frozen —
only a design edit can move this gap, and any such edit re-checks it against
0.032 before anything else in this section.

### 4.4 Shot map

| Shot | From | Angle | Path |
|---|---|---|---|
| left_orbit | RF | **+27.6°** | `plate_l` then `plate_r`, around top |
| right_orbit | **RF** | **-9.0°** | into `cannon_breech` at the mouth: LOAD unlit → kicked straight on up the empty leg and around the top (the loop still counts); LOAD lit → the ball stays for the aim sequence. **Not LF**: at 67.574° that ray passes the `right_ramp` mouth at **0.03100** against the 0.0355 a 0.044 m mouth plus ball needs (**−0.00450**) and grazes `pop_3` at **0.04424** against 0.0445 (**−0.00026**) — and the two lie on *opposite* sides of the line, so the free channel is 0.07524 wide. **Search the channel only across bearings that actually enter the mouth.** 66.320° points straight *at* the wall node `[0.480, 0.888]`, so a bearing just under it (66.31°, where the channel reads a wider 0.07776) misses the aperture outboard and is not a right-orbit bearing at all. The LF bearings that thread the mouth — `perp` ≥ 0.0135 from **both** nodes, i.e. `66.3201 + asin(0.0135/0.84403)` to `68.8306 − asin(0.0135/0.84182)` — are **67.2365°–67.9117°**, and across that band the channel runs 0.07591 down to 0.07456, widest **0.07590** at 67.24°. Against the 0.0800 a ball needs (0.022 mouth + 0.031 cap + 0.027 ball) that is **−0.00410** at the best bearing, so **no** LF bearing threads it. From RF at 80.991° the tightest is `jug_targets` J at 0.03205 ⇒ **+0.00605**, then the mouth's own aperture nodes at 0.01822 ⇒ +0.00472. Shared cause with voltage-vandals' §5.4 row: the LF→right-mouth bearing is 67.574° on every table, so a right-ramp entrance anywhere in the x 0.35–0.38 band lands within a few mm of what a ramp mouth needs |
| left_ramp | RF | **+23.0°** | crosses to right inlane |
| right_ramp | LF | **-26.1°** | crosses to left inlane |
| hoop lane | RF or LF | **+5.8°** (RF) / **-17.1°** (LF) | center spinner at `[0.290, 0.600]` into the pop nest above it |
| ringmaster | **cannon** (195°); off the flippers a lucky RF ricochet only | **—** (no clean flipper line) | left-of-centre standup. RF *does* bear on it — 102.599°, §0.3 angle +12.6°, well inside RF's window — but that ray passes **0.04012** from `pop_2` `[0.254, 0.679215]` against the 0.0445 a 0.031 cap plus ball needs: a **−0.00438** ball margin, and it falls **80.6 %** of the way down the 0.7060 m line, so the straight shot feeds the nest instead. LF is worse — 86.346° passes 0.01938 from the `left_ramp` mouth (**−0.01612**). So no angle is listed: this is the cannon's flattest lit angle (§4.3) and, from a flipper, a ricochet off the nest |
| jug_targets | **RF** | **-15.2°** | right standup trio — **not** LF: the row runs 50.15°–55.93° from the LF pivot, 3.1°–8.9° flatter than its 59° rest end, and 72.53°–76.98° from RF |
| tent | MF (or an aimed cannon shot) | up-left **+45.2°** (pivot line 135.16°, `cannonade_flipper` window 138°→96°) | into the alcove east of `tent_wall`; **`tent_wall` blocks both main flippers' lines** — LF's crosses it at `[0.1062, 0.6214]`, RF's at `[0.1400, 0.6121]`, cone-checked against the scoop's 0.014 m capture disc (§4.3 Tent wall) |

Margins above and in §4.3's cannon table use §2.4's method and carry §0.3's
error bars; the millimetre figures screen for blockage and `shots[<id>].rate`
settles makeability.

### 4.5 Rules

**Skill shot (cannon).** First launch each ball, a **mouth-feed plunge**
(§0.8's stub or soft band, 1.524–1.6028 m/s) loads the cannon and the shot fired from that load is the skill shot. Award
for hitting the aimed target, one per lit angle (§4.3): `ringmaster` (195°)
400,000 · pop nest (226°) 500,000 · `jug_targets` (266°) 600,000 ·
`right_ramp` (252°) 750,000 · tent (204°) 1,500,000. Missing the aimed shot:
100,000 consolation. A full plunge takes the orbit instead and scores no
skill shot — plunge strength *is* the skill here, and the aim is the second
half of it. Every plunge passes up through `feed_deflector` (transparent to
+y, §4.3) and trips its `switch_hit`, so the skill window ignores it exactly
as §0.8 has it ignore the two orbit entry switches and `orbit_merge_gate`.

**Circus acts (modes)** — lit by completing B-I-G (lane-change), started at
`tent_scoop`; each playable once per Finale cycle:

| Act | Timer | Tasks | Awards | Light show |
|---|---|---|---|---|
| Plate Spinner | 40 s | 60 cumulative spins across all 3 spinners | 8,000/spin ×3 during act, 2,000,000 completion | each spinner's plate insert wobbles faster with spin rate |
| Trapeze | 30 s | left_ramp ↔ right_ramp alternating ×4 | 500,000/shot, 2,000,000 completion | white spot chases the crossing ramps |
| Lion Tamer | 30 s | 4 lit standups (J-U-G + rotating 4th on pops), then tent | 300,000 each, tent finale 1,500,000 | pink/amber ring flashes; tent roars |
| Human Cannonball | one shot | LOAD via right_orbit, then cannon at the single lit angle, which steps 195 → 204 → 226 → 252 → 266 → 195… at 4 Hz | hit 2,500,000, miss 250,000 | full blackout except aim arc + the lit target |

**Juggling Multiball.** `plate_l` and `plate_r` share the left orbit lane
(§4.3), so one loop rips both: the pair lights a tent lock every **50**
cumulative spins across the two (doubled during Plate Spinner). Both plates
being in one leg is why 50 is the right number: one loop rips both, so the
count is the same spin *work* a plate-per-leg pair would ask for, delivered on
a single shot. Lock 2 at `tent_lock`; then
LOAD the cannon and fire at the tent = 3-ball. Jackpot alternates strictly
L-side (left_ramp/left_orbit) / R-side (right_ramp/right_orbit): correct
side = 400,000 + 200,000 × streak (max 5); wrong side resets the streak, no
penalty. After a 6-catch streak: Super at `hoop` = 3,000,000 + 20 s spinner
frenzy (25,000/spin). Light show: a "juggled ball" light arcs over the top
at each side swap.

**Combos ("Flight Path").** Ramp then opposite orbit within 3 s: 350,000;
extending with the other ramp: +650,000.

**Wizard — Grand Finale.** Qualify: all 4 acts + Juggling MB. Start at
`tent_scoop`. Phase 1 "Parade" (45 s): all 7 shots lit once each at 600,000
→ Phase 2 "Three-Ring" (3-ball): left_orbit, hoop, right_orbit each need 2
hits, 750,000/hit → Phase 3 "Finale Blast": LOAD cannon; the fired shot,
wherever it lands, scores 10,000,000 with full-field fireworks. Everything
resets.

**Ball save:** 8 s. **Safety Net:** completing B-I-G twice lights one outlane
save.

**Extra balls:** (a) 150 cumulative spinner spins in one ball; (b) complete 2
acts in one ball.

Scoring table (avg game score target 7,000,000):

| Award | Base points | PF-mult? | Avg count/game | % of avg game |
|---|---|---|---|---|
| Switch/rollover base | 4,000 | no | 210 | 12 |
| Spinner spin (base) | 6,000 | no | 140 | 12 |
| Ramp made | 50,000 | no | 12 | 9 |
| Act shots | 300,000–500,000 | no | 4.5 | 24 |
| Act completion | 1,500,000–2,500,000 | no | 0.45 | 12 |
| MB jackpot (with streak) | 400,000–1,400,000 | no | 1.8 | 15 |
| Super + frenzy | 3,000,000 + spins | no | 0.10 | 5 |
| Combos | 350,000–1,000,000 | no | 1.2 | 6 |
| End-of-ball bonus | 8,000 ×spins/10 | no | 3 balls | 3 |
| Grand Finale payoff | 10,000,000 | no | 0.02 | 3 |
| Skill shot (cannon) | 500,000–1,500,000 | no | 2.2 | 9 |

`meta.replay_score` **11,000,000** (§0.6: 1.57 × the 7.0M `score.p50` band
midpoint). High score defaults (10 entries, §0.6; rank 1 = 5 × 7.0M, rank 10
= 7.0M): 1) 35,000,000 TOP · 2) 30,000,000 ACE
· 3) 24,000,000 LEO · 4) 18,000,000 JUG · 5) 14,000,000 POP · 6) 12,500,000
CLW · 7) 11,000,000 BIG · 8) 9,500,000 RNG · 9) 8,000,000 ORB ·
10) 7,000,000 TNT.

### 4.6 Sound / music brief

| Key (§0.9) | Where | Patch | Character |
|---|---|---|---|
| `spinner` | map | `cc_plate` | ceramic ting, pitch per spinner (l/hoop/r = 660/880/1100 Hz) |
| `pop_bumper` | map | `cc_honk` | clown horn, honk pitch via tb.rng ±2 semitones |
| `ball_lock` | map | `cc_tentflap` | canvas whumps ×2 |
| cannon fire | `tb.play_sound` | `cc_boom` | sub kick + noise blast + whistle-up, 700 ms |
| jackpot catch | `tb.play_sound` | `cc_catch` | slap + crowd "ooh" swell (noise formant) |
| streak lost | `tb.play_sound` | `cc_splat` | descending slide whistle + thud |
| act start | `tb.play_sound` | `cc_drumroll` | 900 ms snare roll + cymbal |

Music (12-audio.md §9 reserved song ids): `attract` 90 BPM calliope waltz
(3/4); `main` 124 BPM circus-march synth (oom-pah bass, calliope lead);
`mode` per-act stingers over 128 BPM; `multiball` 148 BPM klezmer-ish frenzy;
`wizard` 84→152 BPM accelerating finale march; `game_over` 2-bar slide-whistle
sag over a lone bass-drum thud.

### 4.7 Difficulty targets

`ball_time_s.p50` 30–42 s · `score.p50` 5.0M–9.0M · `score.p90 ÷ score.p10`
≤ 5.5 · `drains.center` ≤ 0.30 · `shots[tent].rate` 0.08–0.16 ·
`shots[left_ramp].rate` 0.18–0.30 · `modes.started_per_game` 1.8–4.0 ·
`modes.multiball_reach_share` 0.20–0.38 · `modes.wizard_reach_share`
0.01–0.04 · `stuck_balls` 0 (verify the breech never *holds* a second ball
during multiball: the shooter-lane gate is a plain one-way flap with no
script control, so the guard is script-side — while `cannon_breech` is
holding, its `kicker_enter` handler kicks any further ball straight back out
at the 4.0 m/s / 90° default instead of taking a second hold. And every
`tb.kick_hold` path ends in a `tb.kick`: a script-held ball has no framework
auto-eject behind it, only the §4.3 5 s timer). The two surfaces added for the
mini-flipper feed and the tent are checked the same way: `feed_deflector`'s
20° flap and `tent_wall`'s 15.4° slope both beat 08 §1.3's 12.7°
rest threshold, so a ball reaching either one always leaves it — down through
`feed_gate` and off the east end of the tent alcove respectively — and neither
end of either surface leaves a gap in the 0.025–0.032 jam band (§4.3).

`shots[tent].rate`'s 0.08 floor is **not** a widening of the general rule. It
is declared under 14 §8.3's one carved-out exception: a shot reachable *only*
from a mini or upper flipper — not makeable from either main flipper at all —
may floor at 0.08 on the skill-2 `shots[<id>].rate` row instead of 0.10. The
tent qualifies because `cannonade_flipper` is its sole flipper feeder: the
roster's `tent_wall` `[0.075, 0.630]` → `[0.155, 0.608]` crosses **every** line
from either main-flipper pivot into `tent_scoop`'s 0.014 m capture disc — LF
at `[0.1062, 0.6214]`, RF at `[0.1400, 0.6121]`, nearest escape short by
0.0126 m — while the mini flipper's 135.16° line clears the wall's east node
by 0.0215 m of ball margin **and lies inside `cannonade_flipper`'s computed
138°→96° launch window** (`rest_angle_deg` −132: 2.84° of headroom at the
rest end, the scoop's whole ±1.571° capture cone inside it) (§4.3 roster,
§4.3 Tent wall, §4.4). The cannon can reach it too
(204°), but a cannon load is not a flipper shot, so the exception still holds,
and that sole-feeder fact is recorded in `design.md` §Shots as 14 §8.3
requires. Every other bound here, including the
skill-0 `shots[<id>].rate` ≥ 0.02 floor, is declared as written.

### 4.8 Build notes

Built complete in **M17** (second table; `M17b` if split). Requires M6
spinners/gates, M7 kickers/locks, M8 ramps. This is the one table that does
**not** instantiate `plunger_lane`: it ships that prefab's expansion as the
five hand-placed `shooter_*` primitives of §4.3 so `feed_gate` can open a
0.036 m hole in the lane wall. The cannon is a plain `scoop`
driven only by `tb.kick_hold(id)` + `tb.kick(id, speed, angle_deg)`
(10-scripting.md §3.4) with a scripted angle — no VUK, no engine feature
added for it. Its `eject_angle_deg`/`eject_speed` defaults apply only to a
capture the aim sequence never holds (that ball still auto-ejects at
`capture_ms`); once held, the §4.3 script timeout is the only failsafe.

### 4.9 Distinctiveness

Cosmic Carnival is the aiming table: the only one where the player chooses a
launch vector (cannon), the only one with three spinners and a side
mini-flipper shot, and its multiball is a rhythm of alternating sides rather
than a jackpot grind. Its skill ceiling is timing, not endurance.

---

## 5. voltage-vandals — Voltage Vandals

### 5.1 Fantasy

An electric-punk crew knocking over corporate power vaults at night. Every
heist is a timed run under alarm pressure: wrong shots trip the grid and a
magnet snaps the ball dead mid-flight like a searchlight catching you. Loot
is not points until you bank it — push a phase deeper for double, or take
the getaway van and run. The outlane escape hatches save you, but feed the
alarm.

### 5.2 Art

- **Palette:** `midnight-chrome` (13-art-direction.md §2.2, binding
  assignment): near-black asphalt grounds (bg0 `#05070E`), electric arc-blue
  primary (`#4DA6FF`), chrome-gray secondary (`#C4CEDF`), cyan / alarm-red
  accents (`#00F0FF` / `#FF4D6D`), amber `warm` for kickback and danger.
- **Motifs:** (1) exposed cable bundles and arcing tesla filaments (cyan
  accent1) along walls; (2) spray-stencil heist iconography in chrome-gray
  (masks, crowbars, currency bolts); (3) CCTV frames and arc-blue scanline
  cones over the alarm grid.
- **Logo:** "VOLTAGE VANDALS" as dripping spray-paint caps in arc-blue with
  a chrome outline, the V's drawn as lightning bolts; red ALARM lamp
  (accent2) dotting the final "A".

### 5.3 Layout

```
+--------------------------------------+
|         _C1__C2__C3_                 |  camera lanes (skill: hit UNLIT)
|       /(-o-)(-o-)(-o-)\              |
|     /    _________      \ __         |
|    |    | VAULT    |      \G|        |  vault: [123] bank guards
|    |    | [1 2 3]  |       |;|       |  lock scoop K behind it
|    |*   |   (K)L   |       |;|       |  *: fence spinner (left orbit)
|    |;|   \________/        |;|       |  L: vault lock (cap 2)
|    |;|                     |;|       |
|    |;|    M     M          |;|       |  alarm grid magnets ×3
|    |;| %L    M      %R     |;|       |  (triangle, mid-field)
|    |;|  \           /      |;|       |  %L %R ramp entrances
|    |;|   \  T T    / (K2)  |;|       |  T: V-O standups
|    |;|    |       |  van   |;|       |  K2: getaway van scoop
|    |;|    |  T T  |        |;|       |  T: L-T standups
|    |Gk    |       |    Gk  |;|       |  Gk: outlane gate + kickback
|    | /S\  |       |  /S\   |;|       |
|    | \_/ _|       |__\_/   |P|       |
|    |-o-\ /         \ /-o-  |'|       |
|    | \  \   F   F   /  /|            |
|     \ '  \ (L) (R) /  ' /           |
|      \____   DD   ______/           |
+--------------------------------------+
```

Prefabs: standard bottom (§0.4) with each `inlane_outlane_pair`'s
`divider_top` moved 0.002 m inboard (left `[0.064, 0.268]`, right mirrored)
so the outlane channel is ≈ **0.040 m** (hardest table); `orbit` per **§0.8**
(`mouth_x` 0.075, `top_radius` 0.130, `entry_y_left`
**0.620** — **0.030 above §0.8's 0.590 floor**, worth
**2.04317°** of mouth margin against RF's 121° rest end —
`entry_y_right` 0.900) with the §0.8 merge (`orbit_merge_gate`); `top_lanes_n`
n=3 labels C1-C2-C3 ("cameras") `pos` **`[0.330, 0.880]`**, `lane_length`
0.050 (lane centers x 0.290 / 0.330 / 0.370) — the bank sits to the right of
the vault rather than above it, because the §0.8 top band leaves no room for
both; `ramp_standard` ×2 (left ramp → right inlane, right ramp → left
inlane). No pop cluster.

Element roster:

| id | type | pos | key params | notes |
|---|---|---|---|---|
| `vault_bank` | drop_target_bank | 3 targets centered `[0.245, 0.845]`, pitch 0.034 | `facing_deg` 268, `reset` "script" | guards the vault scoop |
| `vault_scoop` | kicker | `[0.245, 0.895]` | `style` "scoop", `eject_angle_deg` -62, `eject_speed` 3.2 | lock / MB start; reachable only while bank is down |
| `vault_lock` | ball_lock | `[0.215, 0.900]` | `capacity` 2, `style` "hidden" | locked balls vanish into the vault; a held ball's top edge sits `0.900 + 0.0135` = 0.9135, **0.0515** under the §0.8 orbit guide's top run at y 0.965 — measure that headroom from the ball's top edge to the guide's run, not from the lock `pos` to anything else, or it reads a spurious 0.0065. Tightest clearance line is the RF `vault_scoop` ray at 0.02918 ⇒ **+0.00918** against its 0.020 m capture radius (§2.4) — the tightest lock figure on any of the five |
| `grid_a/b/c` | magnet ×3 | `[0.165,0.700]`, `[0.260,0.760]`, `[0.350,0.700]` | `strength` 2.2, `radius` 0.080, `default_on` false | alarm grid |
| `van_scoop` | kicker | `[0.430, 0.555]` | `style` "scoop", `eject_angle_deg` 235, `eject_speed` 3.0 | heist start + banking |
| `volt_targets` | standup_target ×4 | V `[0.175,0.615]`, O `[0.310,0.645]`, L `[0.235,0.530]`, T `[0.260,0.490]` | `facing_deg` **288 / 273 / 257 / 282** (V/O/L/T) | V-O-L-T. **Four separately-aimed standups need four separated bearings, and that is what sets these coordinates (computed).** A tighter cluster such as `[0.205,0.610]`/`[0.245,0.625]`/`[0.220,0.520]`/`[0.260,0.535]` has none: from RF those bear 105.15°/100.44°/106.37°/100.65°, so shooting V passes **0.0094** from L and shooting O passes **0.0016** from T, against 0.026 — V and O sit unreachable behind their own partners. Such a cluster also stands in the vault lines: V **0.00645** off the LF→`vault_bank` ray and **0.00199** off the LF→`vault_scoop` ray, T **0.00129** off the RF→`vault_bank` ray. Both tests bind on any re-siting here. Three targets are shot from **RF** at 108.16° (V), 93.13° (O) and 101.90° (T) — 6.3° and 8.8° apart — and **L from LF** at 77.24°, in the 84°–111° band that flipper has entirely to itself; every one of the four clears every other by ≥ **+0.00403**, and the vault lines clear the cluster by ≥ **+0.00446** (§5.4). Each `facing_deg` is its own target's perpendicular to its own arrival. Surface gaps: L–T 0.0222 (≤ 0.024, blocked), every other pair ≥ 0.104 — none in 09 §6's 0.025–0.032 jam band |
| `fence_spinner` | spinner | `[0.029, 0.690]` | `facing_deg` **76** | "the fence", in the left orbit lane on §0.8 keep-out (c)'s spinner offset (0.0085 outboard of the 0.0375 lane center) so the plate's inboard end opens a 0.03387 m escape gap. Crossings: **0.74228 m/s** slowest descent, **1.15109 m/s** slowest climb, against the 0.31 m/s wall floor |
| `left_kickback` | kicker | `[0.088, 0.160]` | `style` "saucer", `eject_angle_deg` 105, `eject_speed` 4.5 | in left outlane channel (fires up-lane along the §0.4 side wall) |
| `right_kickback` | kicker | `[0.392, 0.160]` | `style` "saucer", `eject_angle_deg` 75, `eject_speed` 4.5 | in right outlane channel, mirrored |
| `left_hatch` / `right_hatch` | gate ×2 | `[0.070, 0.205]` / `[0.410, 0.205]` | `facing_deg` 270, `width` 0.040, **`default_state` "open"** | span the outlane channel. `default_state` is **not** left to default: a `one_way` gate is purely mechanical and *ignores* script calls with a warn (09-table-format.md §4.10), which would silently no-op the whole hatch mechanic. Fiction → API: hatch **armed** = `tb.gate_close(id)` (the flap becomes a wall and the outlane cannot drain); hatch **spent/disarmed** = `tb.gate_open(id)` (passable both ways, the outlane drains normally, which is the ball-start state) |
| `left_ramp` | ramp | `[0.140,0.585]` → `[0.408,0.300]` | neon-drift-style profile | "rooftop L". Both entrances moved 0.008 outboard, still mirrored about x 0.260: at `[0.372,0.585]` the right mouth's 0.022 half-width sat **0.03423** off the LF→`right_orbit` ray, 0.0013 under the 0.0355 it needs. At 0.380 that becomes **+0.00612**, and the left mouth still clears the §0.8 left-orbit entry band by **+0.02061** |
| `right_ramp` | ramp | `[0.380,0.585]` → `[0.112,0.300]` | mirrored | "rooftop R" |

**Alarm (scripted, binding).** Level 0–4 (backglass meter + 4 red inserts).
+1: wrong major shot during a heist phase, lit camera lane at launch, buying
a hatch. −1: skill shot, completing V-O-L-T. Ball start: max(0, previous−1).
**Trip:** each +1 with a ball in play fires the next grid magnet in a fixed
`grid_a` → `grid_b` → `grid_c` rotation via `tb.magnet_on` for **1.5 s**
(grab and hold — the three nodes triangulate mid-field, a searchlight
pattern rather than a targeted grab; scripts have no ball-position query),
releasing with a final 60 ms `tb.magnet_pulse` whose decay envelope softens
the drop; red strobe + klaxon during the hold. Alarm 4 = "LOCKDOWN": active
heist fails, unbanked loot lost, alarm resets to 2.

**Hatches (scripted, binding).** Hatches are bought at the getaway van.
While alarm < 4 and `van_scoop` holds a ball (the script takes the ball with
`tb.kick_hold` and owns the release: a `tb.timer` ejects at 1.5 s, since a
script-held ball has no `capture_ms` auto-eject, 08-physics.md §6.9 and
10-scripting.md §3.4), a
flipper-button `switch_hit`
(`button_flipper_left`/`_right`, 10-scripting.md §4.1) before the eject arms
that side's hatch: +1 alarm, `tb.gate_close` on that hatch so the flap blocks
the drain path, kickback armed for one use. Kickback fires on its outlane
`rollover`; the script then disarms with `tb.gate_open`. At alarm 4 the
buttons are ignored — no purchase.

### 5.4 Shot map

| Shot | From | Angle | Path |
|---|---|---|---|
| left_orbit | RF | **+27.6°** | fence spinner, around top |
| right_orbit | LF | **-22.4°** | around top |
| left_ramp | RF | **+23.0°** | → right inlane (entrance `[0.140, 0.585]`, §5.3) |
| right_ramp | LF | **-27.0°** | → left inlane (entrance `[0.380, 0.585]`, §5.3) |
| vault_bank / vault_scoop | LF or RF | **-8.1°** / **+7.3°** (bank), **-7.6°** / **+6.9°** (scoop) | center; scoop only when bank down |
| van_scoop | **RF** | **-11.7°** | right-center scoop — **not** LF: `[0.430, 0.555]` is 56.70° from the LF pivot, 2.30° flatter than its 59° rest end; from RF it is 78.31° |
| volt_targets | **RF** (V, O, T) / **LF** (L) | **+18.2° / +3.1° / +11.9°**, **-12.8°** | four separately-aimed standups at the §5.3 coordinates. L is **not** an RF shot: from RF it bears 106°, behind V — assign all four to RF and two letters have no line at all (§5.3) |

**Clearance record (computed, binding; method and obstacle radii per §2.4,
error bars per §0.3 — this table has the most sub-10 mm rows of the five, so
its `shots[<id>].rate` measurements carry correspondingly more of the verdict).**

| Shot | Tested bearing | Tightest obstacle | ⊥ dist | Need | **Ball margin** |
|---|---|---|---|---|---|
| left_orbit | **118.95683°** (§0.8's true floor for `entry_y_left` 0.620) | `left_ramp` mouth `[0.140, 0.585]` | 0.05343 | 0.0355 | **+0.01793** |
| left_orbit | 121° (RF rest end) | `left_ramp` mouth `[0.140, 0.585]` | 0.07149 | 0.0355 | **+0.03599** |
| right_orbit | 67.574° | right mouth aperture nodes `[0.445,0.900]` / `[0.480,0.888]` (§0.8) | 0.01846 / 0.01847 | 0.0135 | **+0.00496 / +0.00497** |
| right_orbit | 67.574° | `right_ramp` mouth `[0.380, 0.585]` — next, not tightest | 0.04162 | 0.0355 | **+0.00612** |
| left_ramp | 112.948° | `volt_targets` L `[0.235, 0.530]` | 0.06604 | 0.026 | **+0.04004** |
| right_ramp | 63.046° | `van_scoop` `[0.430, 0.555]` | 0.05817 | 0.0275 | **+0.03067** |
| vault_bank LF | 81.892° | `volt_targets` L `[0.235, 0.530]` | 0.03453 | 0.026 | **+0.00853** |
| vault_bank RF | 97.337° | `volt_targets` T `[0.260, 0.490]` | 0.03046 | 0.026 | **+0.00446** |
| vault_scoop LF (bank down) | 82.405° | `volt_targets` V `[0.175, 0.615]` | 0.03238 | 0.026 | **+0.00638** |
| vault_scoop RF (bank down) | 96.872° | C1 divider lower end `[0.270, 0.855]` | 0.02003 | 0.0135 | **+0.00653** |
| van_scoop | 78.315° | `volt_targets` T `[0.260, 0.490]` | 0.15331 | 0.026 | **+0.12731** |
| volt V (RF) | 108.159° | `volt_targets` L `[0.235, 0.530]` | 0.03052 | 0.026 | **+0.00452** |
| volt O (RF) | 93.132° | `right_ramp` mouth `[0.380, 0.585]` | 0.06662 | 0.0355 | **+0.03112** |
| volt L (LF) | 77.238° | `volt_targets` T `[0.260, 0.490]` | 0.03322 | 0.026 | **+0.00722** |
| volt T (RF) | 101.896° | `right_ramp` mouth — nothing nearer | 0.13046 | 0.0355 | **+0.09496** |

Two conventions this table is easy to get wrong, both binding. **Test an orbit
row at §0.8's true floor, never above it.** For `entry_y_left` 0.620 that floor
is **118.95683°** (`117.5993 + 1.3575`, §0.8) — a nominal "floor + slack"
bearing such as 119.26° understates the worst case, because the ray flattens
toward the floor. **Count the right mouth's own aperture nodes on every
`right_orbit` line.** On the 67.574° ray they bind first, at **+0.00497**,
ahead of the `right_ramp` mouth's +0.00612; cosmic-carnival's §4.4 counts them
on its RF line and neon-drift's §1.4 on its LF one, and every table must.
`vault_lock` is measured at 08 §6.14's 0.020 m capture radius (§2.4):
**+0.00918** on the RF `vault_scoop` line, clear of the +0.003 floor and behind
that row's C1 divider end.

`vault_scoop` keeps its "only when the bank is down" qualifier from the row
above — that is a rules gate, not a clearance one; its margins here are
measured with the bank dropped, as the shot is only ever taken that way.

### 5.5 Rules

**Skill shot (inverted).** A "camera" light sweeps C1-C2-C3 at 2 Hz. Soft
plunge into the **unlit** lane: 750,000 and alarm −1. Hitting a lit lane:
alarm +1, no award.

**Heists (modes)** — lit by spelling V-O-L-T; started at `van_scoop`. Each
has a Job phase and optional Escape phase. Correct shots feed a **loot pot**
(backglass); wrong major shots trip the alarm (§5.3). On Job completion,
choose within 10 s: `van_scoop` banks the pot into score, or the flashing
"push on" shot enters Escape (pot ×2 potential). Timer expiry loses half the
pot (rounded down); Lockdown loses all of it.

| Heist | Job (timer) | Escape (timer) | Loot | Light show |
|---|---|---|---|---|
| Substation Job | left_orbit → left_ramp → vault_bank complete → van (45 s) | orbits ×3, any order (20 s) | 400,000/shot; Escape doubles pot | arc-blue sparks trace completed legs |
| Vault Crack | vault_bank ×2 complete, then vault_scoop (40 s) | ramps ×2 alternating (15 s) | 500,000/target | vault inserts crack open progressively |
| Rooftop Run | left_ramp ↔ right_ramp ×4 alternating (35 s) | one orbit within 8 s (8 s) | 450,000/ramp; wrong ramp = alarm | cyan chase across both ramps |

**Smash & Grab Multiball.** Complete `vault_bank` outside a heist to open the
vault (bank stays down 20 s); `vault_scoop` locks (capacity 2). Two locks,
then `vault_scoop` again = 3-ball. Jackpots 600,000 at the four lit shots
(both ramps, both orbits); grid magnets fire seeded random 300 ms grabs every
3–5 s during MB (chaos only — no alarm cost). After 4 jackpots: Super
2,500,000 at `van_scoop`. Light show: rolling blue/cyan raster over the
whole field; red flash on each grid grab.

**Combos ("Clean Getaway").** 3 different major shots within 8 s with no
alarm trip: 500,000.

**Wizard — Blackout Job.** Qualify: bank all 3 heists (any amount) + play
Smash & Grab. Start at `van_scoop`. Phase 1 "Cut the Power" (30 s): 5 lit
shots, 600,000 each, each kills one grid magnet / GI section (field darkens
progressively) → Phase 2 "Blackout" (3-ball): near-dark field, only arrows
lit; every lit shot 750,000 and feeds a pot seeded with 5,000,000 + banked
loot ÷ 10 → Phase 3 "The Fence" (10 s, last ball): bank the pot at
`van_scoop` or lose half. Payoff: the banked pot (typically 8–15M). Full
reset after.

**Ball save:** 6 s (shortest). No free outlane saves — hatches only.

**Extra balls:** (a) bank a single heist worth ≥ 4,000,000; (b) complete
V-O-L-T twice in one ball.

Scoring table (avg game score target 10,000,000; deliberately high variance):

| Award | Base points | PF-mult? | Avg count/game | % of avg game |
|---|---|---|---|---|
| Switch/rollover base | 5,000 | no | 190 | 10 |
| Spinner ("fence", per spin) | 10,000 | no | 50 | 5 |
| Ramp made | 60,000 | no | 13 | 8 |
| V-O-L-T target | 60,000 | no | 14 | 8 |
| Heist loot banked | 400,000–500,000 per shot, ×2 on Escape | no | 5.5 shots | 30 |
| MB jackpots | 600,000 | no | 1.4 | 8 |
| Super jackpot | 2,500,000 | no | 0.10 | 3 |
| Clean Getaway | 500,000 | no | 1.0 | 5 |
| End-of-ball bonus | 20,000 ×banked heists ×targets/10 | no | 3 balls | 5 |
| Blackout Job payoff | ~10,000,000 pot | no | 0.03 | 3 |
| Vault open / lock | 250,000 | no | 2.0 | 5 |
| Skill shot | 750,000 | no | 1.3 | 10 |

`meta.replay_score` **16,000,000** (§0.6: 1.6 × the 10.0M `score.p50` band
midpoint). High score defaults (10 entries, §0.6; rank 1 = 5 × 10.0M, rank 10
= 10.0M): 1) 50,000,000 VLT · 2) 40,000,000 AMP
· 3) 31,000,000 OHM · 4) 24,000,000 ARC · 5) 18,000,000 WAT · 6) 16,000,000
ION · 7) 14,500,000 ZAP · 8) 13,000,000 JLT · 9) 11,500,000 KEY ·
10) 10,000,000 CAP.

### 5.6 Sound / music brief

| Key (§0.9) | Where | Patch | Character |
|---|---|---|---|
| `drop_target` | map | `vv_bolt` | bolt-cutter snap |
| `magnet` | map | `vv_klaxon` | two-tone alarm + electric crackle; the engine retriggers it every 500 ms, covering the full 1.5 s grab |
| `kicker` | map | `vv_surge` | capacitor charge-up 300 ms + discharge boom (kickbacks and scoops) |
| magnet release | `tb.play_sound` | `vv_zapoff` | reverse zap + bass thud |
| loot shot | `tb.play_sound` | `vv_cash` | register ka-ching (metal ping ×2) + coin shimmer |
| bank at van | `tb.play_sound` | `vv_vandoor` | sliding door slam + engine rev away |
| lockdown | `tb.play_sound` | `vv_shutdown` | power-down sweep 2 kHz→60 Hz, 900 ms, then silence beat |
| hatch purchase | `tb.play_sound` | `vv_hatch` | servo + spark shower |

Music (12-audio.md §9 reserved song ids): `attract` 80 BPM dark synth pulse
with radio chatter blips; `main` 126 BPM electro-punk bass riff; `multiball`
145 BPM breakbeat; `wizard` Phase 1 132 BPM stripped, Phase 2 near-silent sub
+ heartbeat, Phase 3 full 150 BPM payoff drop; `game_over` 2-bar siren
receding into radio static. This table maps no `mode` song: it defines the
extra (non-reserved) song id **`heist`** — 132 BPM + ticking-clock percussion,
gaining a dissonant layer per alarm level — and the script plays it with
`tb.play_music("heist")` when a heist starts (12-audio.md §9 allows extra
song ids played *instead of* `mode`), returning to `main` on bank, expiry, or
Lockdown.

### 5.7 Difficulty targets

`ball_time_s.p50` 24–34 s (hardest) · `score.p50` 6.5M–13.5M ·
`score.p90 ÷ score.p10` 5.0–8.5 (high variance is the design; below 5.0 means
banking is too safe — raise Escape multiplier exposure per §0.7 order after
geometry steps) · `drains.center` ≤ 0.34 · `shots[vault_scoop].rate`
0.12–0.22 · `shots[van_scoop].rate` 0.18–0.30 · `modes.started_per_game`
1.5–4.0 · `modes.multiball_reach_share` 0.12–0.28 ·
`modes.wizard_reach_share` 0.005–0.03 · `stuck_balls` 0 (grid grabs must
always release: watchdog — if a grid magnet has been on > 2.0 s, force
`tb.magnet_off`).

### 5.8 Build notes

Built complete in **M17** (third table; `M17c` if split). Requires M6 gates,
M7 kickers/banks/locks, M8 magnets. The hatch purchase reads flipper-button
`switch_hit`s during the `van_scoop` hold (10-scripting.md §4.1) — no new
input paths or engine features. The alarm-grid grab is
`tb.magnet_on`/`tb.magnet_off` with a rules-side watchdog timer — never an
engine timer.

### 5.9 Distinctiveness

Voltage Vandals is the risk table: shortest ball save, widest outlanes, and
the only table where points are provisional — loot is worthless until banked,
and safety is bought with the same currency (alarm) that kills your modes.
Where the other four escalate by adding lit shots, this one punishes wrong
ones: the alarm grid makes *not shooting* a skill. Highest variance and the
most player choice per ball of the five.

---

## 6. test-lab

`test-lab` is the minimal valid table used by tests and docs; its complete
definition and source of truth is 09-table-format.md (with rules examples in
10-scripting.md) — it is not designed here and must not be shipped in the
table-select menu.

---

## Common pitfalls

- **Making a listed shot geometrically impossible.** Every shot-map shot must
  be makeable from its listed flipper: verify its `shots[<id>].rate` (14
  §8.2 report) ≥ the table's floor before writing rules on it. The tent
  (cosmic-carnival) must be impossible from the main flippers yet reliable
  from the mini flipper.
- **Mistaking a clearance margin for proof that a shot lands.** Every margin
  in this document comes from §0.3's straight-ray model, which omits path
  droop under slope gravity (≈ 5.6 mm on a 5 m/s shot over 0.50 m, ≈ 10 mm on
  a 4 m/s shot over 0.55 m) and the offset of the real launch point along the
  bat face — both larger than a typical margin. Below ~10 mm the number is
  **indicative**: it screens for gross blockage and tells you which element
  may not move, and `shots[<id>].rate` from the §0.7 `tb_autoplay` suite is
  what decides makeability. Never retune geometry off a paper margin alone.
- **Magnet holds instead of pulses.** Crane and conveyor (tilt-o-tron) use
  timed `tb.magnet_pulse` chains; only the alarm grid (voltage-vandals) uses
  sustained `tb.magnet_on`, guarded by the 2.0 s script watchdog. A magnet
  left on is a stuck ball and fails `stuck_balls` 0.
- **Placing orbit furniture off the lane center.** An orbit hugs the
  boundary, so with §0.8's `mouth_x` 0.075 the lane centers are
  `mouth_x/2` = 0.0375 and `W − mouth_x/2` = 0.4825. Do **not** compute them as
  if the lane were inset from the wall (0.0975 / 0.4225): those x's fall
  outside the lane entirely, and a magnet,
  lock or light that misses the center line simply misses the ball. A
  `spinner` is the one deliberate exception: its 0.025 m plate is offset 0.0085
  **outboard** (x 0.029 / 0.491) and leaned 14.0°, because a plate centred in a
  0.075 m lane leaves 0.02537 m at each end — under the 0.027 a ball needs — and
  so becomes a pocket for any ball that stops on it (§0.8 keep-out (c)). It
  still covers 0.04113 m of the lane's 0.048 m of ball-centre travel. The same arithmetic sets the top
  band: nothing on layer 0 sits above y 0.913 between x 0.130 and 0.390, and
  the orbit's inner guide runs 0.052 m above that.
- **Trusting gravity to un-stick a nearly level surface.** In-plane gravity
  is only `g·sin 6.5° = 1.1105 m/s²`, while rolling resistance is
  `0.025·g·cos 6.5° = 0.2437 m/s²` (08-physics.md §1.3): a surface must lean
  more than **12.7°** in the table plane before gravity alone will restart a
  stopped ball. Every "the tilt rolls it back into play" failsafe here is
  therefore a magnet, a kicker, or a script retry — never the tilt (see the
  tilt-o-tron crane rail, §3.3).
- **Leaving a scripted gate at its default state.** A `gate` with no
  `default_state` is `one_way`, and a `one_way` gate ignores
  `tb.gate_open`/`tb.gate_close` with a warn (09-table-format.md §4.10) — the
  mechanic silently does nothing. Every gate a table drives (voltage-vandals'
  hatches) declares `default_state` explicitly.
- **Reading a captive-ball full-travel flag off `switch_hit`.** There is no
  such flag: the strike emits `switch_hit{id, ball_id, speed, tags}` and the
  captive ball reaching its far end at ≥ 0.3 m/s emits the separate canon
  event `captive_full_travel{id}` (PLAN.md §5.7). Atomic Diner's SHAKE and
  blender counting use the latter; the score-per-hit row uses the former.
- **Expecting edits to generated prefab elements to stick.** They never do
  (09-table-format.md §5). A table that needs a hole in a prefab wall copies
  that prefab's documented expansion into `elements` and deletes the instance
  — the only place this is done is cosmic-carnival's shooter lane (§4.3), and
  it is called out there. Copying means running the prefab's *arithmetic*
  (09 §5.2: `xw = W − lane_width` = 0.480, `y_gate = top_y + 0.008` = 0.888),
  not eyeballing its picture: a lane wall topping out at 0.880, a top post
  centred on the wall line, or an exit gate at 0.895 each breaks something
  the prefab existed to get right.
- **A drop bank whose `facing_deg` and `targets` disagree.**
  `drop_target_bank` takes an explicit `targets` array (09-table-format.md
  §4.8), so nothing derives the row from the facing — endpoints, pitch and
  each target's `facing_deg` are three independent numbers the author must
  make agree. Pitch is the center-to-center step **along the row**
  (`span ÷ (n − 1)`), never its x component, and `facing_deg` is the row's
  outward normal, which must point back down-table along the line of the
  flipper the §x.4 shot map says shoots the bank — or at worst within ±90° of
  it, which is what still makes it a face hit (§3.3 `legs_bank` sits 22.6°
  off). Recompute all three whenever an endpoint moves (§1.3 `gear_bank`,
  §3.3 `torso_bank` / `legs_bank`).
- **Assigning a shot to a flipper that cannot launch into it.** A flipper's
  reachable bearings are exactly `rest − 90` → `rest − 90 − swing` ("right")
  or `rest + 90` → `rest + 90 + swing` ("left") — 52° wide for the standard
  pair (§0.4), which is far narrower than a layout picture suggests. Elements
  near a side wall at mid height are usually shot from the **far** flipper,
  not the near one — §1.4 `pit_scoop`, §2.4 `order_window` / `bur_targets` /
  `ger_targets`, §3.4 `legs_bank`, §4.4 `jug_targets` and §5.4 `van_scoop` are
  all cross-field shots, and each row says so explicitly because the near
  flipper is the intuitive wrong answer. Compute `bearing(pivot → aim point)`
  per §0.3 and check it against the window before writing a §x.4 row.
- **Shipping without `meta.replay_score`.** The framework falls back to
  5,000,000 (11-game-framework.md §3.3), which is under every table's
  `score.p50` band here: nearly every game would collect a replay extra ball
  and the §x.7 gated metrics would drift. Each §x.5 states the value.
- **Forgetting layer filters.** Ramps and the diner counter are `layer` 1;
  layer-0 magnets (drift corner, grid, conveyor) must not attract balls on
  ramps above them — the sim couples same-layer only (08-physics.md).
- **Lock capacity vs. trough arithmetic.** The trough holds 4 balls (canon
  §5.3). Tilt-O-Tron's 4-ball MB uses all of them: with 3 locked, exactly 1
  is in play — never auto-serve a replacement while locks are full. In
  multiplayer, locks are virtual per 11-game-framework.md.
- **Applying multipliers to the wrong rows.** Playfield multipliers apply
  exactly to rows marked "PF-mult? yes" — never to bonus, wizard payoffs, or
  fixed super jackpots.
- **Drop banks resetting under a ball.** Reset via `tb.drop_bank_reset` from
  a `tb.timer` no shorter than the 1.5 s post-`bank_complete` delay — at
  these geometries long enough for any rejected ball to clear the face — and
  never from inside a `target_down` handler. Scripts have no ball-position
  query; if a deterministic replay test ever shows a target rising under a
  ball, lengthen the delay rather than resetting sooner.
- **Skill-shot windows that never close.** Every skill shot ends at the first
  switch hit after launch or 6 s after `ball_launched`, whichever is first
  (flipper-button presses count — they are `switch_hit`s, 10-scripting.md
  §4.1). One exemption, binding: the two `orbit` entry switches and
  `orbit_merge_gate` do **not** close the window — every plunge trips the
  merge gate on the way out of the shooter lane, and a mouth-feed plunge trips
  it again on the way back out of the right mouth — plus the right entry
  switch, for any that got above y 0.920 — so
  without the exemption a skill shot would end itself before the ball was
  ever in play (§0.8).
- **Scoop eject into the drain.** Every `eject_angle_deg` here points at a
  flipper, except the deliberately aimed ones — `cannon_breech`
  (cosmic-carnival) fires at its lit target, the outlane kickbacks fire
  up-lane, and `crane_dock` (tilt-o-tron) fires up into the rail. After any
  §0.7 geometry tuning, re-verify each feeding kicker's eject lands on a
  flipper in ≥ 95% of 200 seeded ejects, and recompute the §4.3 bearing table
  — every cannon angle must still reach its named target with the listed wall
  clearance, and adjacent lit angles must still differ by ≥ 8°.
- **Wizard progress surviving reset.** "Resets after" is binding. Per-player
  state lives in `tb.state`, never Lua globals, or player swaps leak progress.
- **Ball save vs. drain ordering.** `drain` fires before the ball-save
  decision (11-game-framework.md): award bonus / end modes on `ball_end`,
  not `drain`.
- **Hardcoding music transitions.** Use `tb.play_music(song_id)` with each
  table's §x.6 ids — the reserved ones are 12-audio.md §9's
  `attract`/`main`/`mode`/`multiball`/`wizard`/`game_over` (there is no
  `base`), plus any extra table song such as voltage-vandals' `heist`;
  restore `main` when multiball/modes end.
- **Inventing default high scores.** Use exactly the ten seeded entries per
  table; they are calibrated from ~5× the table's `score.p50` at rank 1 down
  to ≈ `score.p50` at rank 10, so real players can reach the board.

## Done when

- [ ] All five `tables/<slug>/` directories exist with exactly `table.json`,
      `rules.lua`, `art.json`, `audio.json` (no `assets/` needed).
- [ ] `tb_validate` passes with zero errors and zero warnings on all five.
- [ ] Every roster element exists in `table.json` with the listed id, type,
      and non-default params; prefab instances match §0.4 + each prefab list.
- [ ] Every table's `orbit` instance passes §0.8's `mouth_x` 0.075 /
      `top_radius` 0.130, its outer wall uses the same corner radius, every
      orbit-lane element sits on x 0.0375 or 0.4825 — **except a `spinner`,
      which sits on §0.8 keep-out (c)'s offset x 0.029 / 0.491 at
      `facing_deg` 76 / 104** — no layer-0 element
      **between the legs — x 0.130–0.390, §0.8's top band, not the lanes'
      own x-bands, which legitimately carry the orbit's right entry switch
      at y 0.920** — sits above y 0.913, and the §0.8 `orbit_merge_gate`
      exists, starting on the shooter wall's top node at y 0.888
      (cosmic-carnival additionally ships the §4.3 hand-placed shooter lane
      instead of the `plunger_lane` prefab, at 09 §5.2's own arithmetic —
      wall top and gate line 0.888, **no top post** (09 §5.2's merged
      variant), gate `[0.500, 0.888]` — with `feed_gate`'s aperture 0.036),
      and no table places a solid collider in the merge gate's V014 ray
      corridor, the right lane between y 0.8945 and y 0.9231, nor a
      **spinner** anywhere in the merged right lane above the gate line
      y 0.888 (§0.8 keep-out (b) — 08 §6.6's slow-pass wall), and every
      spinner that is in a lane clears §0.8 keep-out (c): offset x, 14.0°
      lean, 0.03387 m inboard escape gap, and a recorded slowest crossing
      above the **0.31 m/s** wall floor in each direction its leg carries.
- [ ] Every shot-map shot meets its `shots[<id>].rate` floor and the tent shot
      is unmakeable from the main flippers, **measured** by the §0.7 protocol —
      the §x.4 clearance records are §0.3's straight-ray screening, not
      evidence for this box, and where the two disagree the measurement wins
      and the geometry is retuned by §0.7's steps.
- [ ] All §x.7 difficulty targets pass on one uninterrupted §0.7 suite per
      table (three 500-run `--balls 3` sweeps at skills 0/1/2 + the single
      skill-1 300 s coverage session; `--runs 20` iteration numbers never
      count, §0.7), with `stuck_balls` 0, and the same numbers are declared
      in that table's
      `meta.autoplay_bounds` — each bound carrying the skill §0.7 assigns it
      and narrowing (never widening) its 14 §8.3 row — so both
      `tb_autoplay --check-bounds` jobs, the `--seconds 300` smoke and the
      `--balls 3` run, are green.
- [ ] Each `table.json` declares the §x.5 `meta.replay_score`
      (12M / 9M / 14M / 11M / 16M), never the 5,000,000 framework fallback.
- [ ] A scripted rules test (16-testing-ci.md harness) drives each table
      through skill shot, every mode, every multiball with all jackpot types,
      one combo, and wizard qualify/start/complete — asserting the exact
      award values of the scoring tables.
- [ ] Dedicated deterministic replay tests exist for: crossing ramps + drift
      magnet curve + an unlit `drift_lock` releasing a left-orbit ball onward
      (neon-drift), the `counter_drop` return and a flipper shot that fails to
      bind to its seam (atomic-diner), crane dock → feed gap → magnet hand-off
      into the bay plus the 2.5 s re-run after a forced stall (tilt-o-tron),
      cannon hold → aim → button fire at each of the five §4.3 angles and the
      5 s script timeout release (cosmic-carnival), 1.5 s alarm grab + hatch
      `tb.gate_close`/`tb.gate_open` purchase cycle (voltage-vandals).
- [ ] Default high-score entries match §x.5 exactly on first boot: exactly ten
      per table in `meta.default_scores`, V028-clean, rank 1 ≈ 5 × and rank 10
      ≈ 1 × that table's §x.7 `score.p50` band midpoint (40/30/45/35/50 M down
      to 8/6/9/7/10 M).
- [ ] Each `audio.json` defines every patch and every song id named in its
      §x.6 — the 12-audio.md §9 reserved state ids it maps plus any extra
      table song (voltage-vandals' `heist`); unlisted events fall through to
      the built-in bank. Every §x.6 row marked "map" is an `audio.json` `map`
      entry keyed by a 12 §7.2 purpose, every row marked `tb.play_sound` is
      cued from `rules.lua` (§0.9), and no row invents a purpose key.
- [ ] Each `art.json` uses its assigned 13-art-direction.md palette and
      renders its logo treatment on the backglass.
- [ ] Build-notes milestones honored: neon-drift matches §1.8 at each of
      M5/M6/M7/M8/M9/M13/M14; atomic-diner lands in M16; the rest in M17.
- [ ] `test-lab` remains exactly as defined by 09-table-format.md.
