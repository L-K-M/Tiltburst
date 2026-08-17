# 14 — Authoring Guide

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 08-physics.md, 09-table-format.md, 10-scripting.md,
11-game-framework.md, 12-audio.md, 13-art-direction.md. The worked example in
§10 must stay consistent with Atomic Diner in 15-launch-tables.md.

This document is the procedure for creating a complete, fun, **different**
Tiltburst table. Its primary reader is an LLM (multimodal when available)
authoring a table from scratch; it is equally usable by a human. A table is
four plain-text files (PLAN.md §5.5): `table.json`, `rules.lua`, `art.json`,
`audio.json`. This guide tells you what to put in them, in what order, and
how to know when you are done.

A great table has three properties, in priority order:

1. **Fair** — every drain feels caused by the player, not the layout.
2. **Legible** — at any moment the player knows what to shoot and why.
3. **Distinct** — remove the signature mechanic and the table stops
   making sense (§3).

Working notes live in `tables/<slug>/design.md`; tools ignore pack files
other than the four canonical ones and `assets/`. `design.md` ships with
the table as its design record.

## 1. The 12-step workflow

Follow the steps in order. Each step names its command(s) and the artifact
it must produce before the next step starts. Steps 5, 6, and 11 are loops:
run them until their exit condition holds.

| # | Step | Command(s) | Artifact |
|---|------|-----------|----------|
| 1 | Concept | none (writing) | `design.md` §Concept: 3-sentence pitch |
| 2 | Theming worksheet | none (writing) | `design.md` §Theme: filled worksheet per 13-art-direction.md |
| 3 | Table identity | none (writing) | `design.md` §Identity: signature + 2 supports + wizard (§3) |
| 4 | Layout blockout | edit `table.json` | table.json with prefabs, all elements placed, shot map in `design.md` §Shots |
| 5 | Validate loop | `tb_validate tables/<slug>` | exit code 0, zero errors |
| 6 | Greybox playtest | `tb_autoplay tables/<slug> --runs 20 --skill 1 --seed 1000 --balls 3 --report ap1.json` **and** `tb_autoplay tables/<slug> --runs 1 --skill 1 --seed 1000 --seconds 300 --report ap1-cov.json` | geometry metrics in range (§8.3, layout rows only; each read from the shape §8.3 assigns it) |
| 7 | Rules skeleton | edit `rules.lua`, rerun step 5 | every switch scores; ball flow works start-to-finish |
| 8 | Full rules | edit `rules.lua` | all modes, multiball, wizard implemented; economy table in `design.md` §Economy |
| 9 | Art pass | edit `art.json`; `tb_screenshot tables/<slug> --views full,lower,upper,backglass,attract --out review/` | screenshots pass §6 checklist |
| 10 | Audio pass | edit `audio.json` | every event row in `design.md` §Audio mapped to a patch; music states covered (§7) |
| 11 | Tuning loop | `tb_autoplay --balls 3` at `--skill 0`, `--skill 1`, `--skill 2`, plus one `--skill 1 --seconds 300` session | every metric green at the skill **and** in the session shape §8.3 assigns it |
| 12 | Ship checklist | all tools | every §11 item checked, recorded in `design.md` §Ship |

Rules for the workflow:

- Never advance with a failing exit condition. A layout that fails autoplay
  geometry metrics must be fixed **before** rules are written on top of it —
  rules cannot rescue a bad layout. Never edit `art.json` before step 9.
- Re-run `tb_validate` after **every** file edit in steps 7–11; it validates
  all four files, not just `table.json`.
- Each loop iteration changes at most 3 parameters, then re-measures. Bulk
  changes make the tuning matrix (§8.4) unusable — metric movement becomes
  unattributable.
- **Two run counts, and they are not interchangeable.** Every `--runs 20` in
  this guide (steps 6 and 11, §5.1's EGS, §10) is the **iteration** count:
  fast feedback while you change parameters. The **binding acceptance** run
  is 15-launch-tables.md §0.7's per-table suite — three `--runs 500 --balls 3`
  sweeps at skills 0, 1, 2 plus one `--runs 1 --skill 1 --seconds 300`
  coverage session, all on one `--seed`, with every target green on one
  uninterrupted suite. Iterate at 20, ship on 500 (§11 item 2); a table is
  never declared green on a 20-run report.

## 2. Step details

### Step 1 — Concept

Write exactly three sentences in `design.md` §Concept: (a) theme and mood,
(b) the one thing the player does here that no other table offers, (c) the
wizard-mode fantasy. If (b) describes something any pinball table does
("hit ramps to start multiball"), start over.

### Step 2 — Theming worksheet

Fill the theming worksheet from 13-art-direction.md into `design.md` §Theme.
At minimum it fixes: theme statement, palette selection (from the 13 palette
sets), 3–5 iconography motifs, the naming vocabulary (what lights, modes, and
shots are called — every name must come from the theme), and the music
adjective pair (e.g. "greasy + optimistic"). Every later artifact reuses
these words; a mode named "Mode 2" is a legibility failure.

### Step 3 — Table identity

Apply the identity formula of §3. Artifact: `design.md` §Identity, four
lines filled in, distinctiveness test answered in writing.

### Step 4 — Layout blockout

Pick a skeleton (§4.1), instantiate its prefab list in `table.json` (prefab
schemas in 09-table-format.md), then place the signature-mechanic elements
using the shot map method (§4.2). Artifact: a `table.json` where every
non-prefab element has a comment naming its shot label, plus the shot map
table in `design.md` §Shots.

### Step 5 — Validate loop

Run `tb_validate tables/<slug>`; fix every error (§8.1). Warning policy:
§11 item 1.

### Step 6 — Greybox playtest

Run **both** step-6 autoplay commands from the §1 table — the `--balls 3`
sweep and the 300 s coverage session; each layout metric is read from the
shape §8.3 assigns it (`ball_time_s.p50`, `drains.*`, `coverage.share`,
`stuck_balls` from the 300 s session; `shots[<id>].rate` from `--balls 3`).
Only the layout metrics matter here; score and mode metrics are meaningless
before rules exist. Apply the tuning matrix (§8.4) until the layout rows are
green.

### Step 7 — Rules skeleton

Write the minimal `rules.lua`: `tb.on("switch_hit", ...)` scoring every
switch its base value, `ball_start`/`drain`/`ball_end` flow, ball save via
`tb.ball_save`, one placeholder mode (full API in 10-scripting.md). Exit
condition: a 3-ball game completes in `tb_autoplay` with a nonzero score
and `script_errors` = 0 in the report.

### Step 8 — Full rules

Implement the economy (§5.1), mode architecture (§5.2), multiball (§5.3),
and wizard. Artifact: the filled economy budget table in `design.md`
§Economy, plus rules card text — 3–6 short lines shown on the backglass
rules page, stored in `table.json` `meta.rules_card` (09-table-format.md).

### Step 9 — Art pass

Three sub-passes in order, per 13-art-direction.md: blockout colors →
themed art → glow polish (§6). After each sub-pass run `tb_screenshot` and
review against the §6 checklist — visually if multimodal, else via the
validator's art warnings and the 13-art-direction.md contrast table.

### Step 10 — Audio pass

Map every scoring event to a synth patch, compose the music, plan the
music states — method in §7, formats in 12-audio.md.

### Step 11 — Tuning loop

Run autoplay three times in the `--balls 3` shape — `--skill 0 --seed
2000`, `--skill 1 --seed 2001`, `--skill 2 --seed 2002`, each `--runs 20`
with `--report s<skill>.json` — plus one `--skill 1 --seconds 300 --runs 1
--seed 2001 --report cov.json` coverage session, until every metric is
green at the skill and in the shape §8.3 assigns it. Use the tuning matrix
(§8.4); change ≤ 3 parameters per iteration; keep every report file — the
report sequence is the tuning log. `--runs 20` here is the iteration count
(§1); once the loop is green, re-run the same three sweeps at `--runs 500`
on a single seed — 15-launch-tables.md §0.7's acceptance suite — and it is
that suite's numbers that go in `design.md` §Ship and in the table's §x.7.

### Step 12 — Ship checklist

Work through §11 item by item; record the result of each in `design.md`
§Ship. A table ships only with every item checked.

## 3. Table identity formula

Every table is exactly:

```
identity = 1 signature mechanic  (something the player has not seen before)
         + 2 supporting systems  (familiar pinball systems that feed it)
         + 1 wizard payoff       (the mode everything builds toward)
```

Write these as four lines in `design.md` §Identity. The supporting systems
must **feed** the signature mechanic (progress it, gate it, or bank value
into it), not run beside it.

**The distinctiveness test (mandatory, in writing):** delete the signature
mechanic from the design. If the remaining table is still a coherent,
shippable pinball table, the mechanic was decoration — redesign until
removing it leaves a hole the rules cannot function without.

### Signature mechanic seeds

Ten seeds. Use one, combine two, or invent your own of the same ambition.
Two shipped tables may never share a signature mechanic (PLAN.md §5.8).

| # | Seed | Sketch |
|---|------|--------|
| 1 | Magnet chains | A row of 3+ magnets (`magnet`) pulsed in sequence by rules carries the ball along a path no flipper could send it; timing windows tighten each chain level |
| 2 | Reversible flippers | A mode swaps flipper button mapping (`tb.set_flipper_enabled` on paired flippers at mirrored positions) for a timed scoring frenzy; lights must telegraph it 3 s ahead |
| 3 | Traveling hot target | A bank of 4–6 static `standup_target`s in which the *hot* one marches: a script timer (`tb.timer`) steps the lit target one position every 700 ms (`tb.light_on`/`tb.light_off` on the per-target inserts) and a `magnet` in front of the bank fires a short `tb.magnet_pulse` to bend a rolling ball onto the lit face; hitting the hot target scores the major value and reverses the march, hitting a cold one scores minor. Nothing physically moves — the *designation* moves. There is no toy kinematics API: a `toy` is a static collider plus art animation hooks (09-table-format.md §4.20), so a mechanically moving target is not buildable in v1 |
| 4 | Risk-gated outlanes | Player-controlled `gate` elements on the outlanes: open = 5× outlane-adjacent scoring but real drain risk; closed = safe, base value |
| 5 | Spinner-charged bank | Spinner spins charge a meter; the drop bank only raises (becomes shootable) above a charge threshold and lowers as charge decays at 5 %/s |
| 6 | Captive-ball ladder | One `captive_ball` whose captive travel distance is the progress bar: each solid hit advances it one notch; a soft hit resets a notch |
| 7 | Ball-swap kicker | A `kicker` holds the ball while a second ball plays (`tb.add_ball`); scoring during the hold multiplies the held ball's release jackpot |
| 8 | Orbit momentum | Consecutive orbits without a flipper touch build a speed multiplier read from actual ball speed (`switch_hit` payload velocity); one flip banks it |
| 9 | Shot roulette | Every 20 s a scripted light sweep re-randomizes (via `tb.rng`) which single shot is "hot" for 10× value; pattern learnable within a game |
| 10 | Sacrificial locks | Locked balls (`ball_lock`) can be *spent* one at a time to buy mode time or bail out a tilt warning, instead of only starting multiball |

Supporting systems are chosen from the ordinary vocabulary: a drop bank, a
pop cluster, top lanes with lane-change, a horseshoe, an inner loop, a
kickback, a skill shot. Two is the number: one progresses the signature, one
banks value from it.

## 4. Layout design method

### 4.1 Skeleton recipes

Start every layout from one of these four skeletons. A skeleton is a
starting arrangement, not a template to ship: after instantiating it you
must add/replace elements for your signature mechanic and re-derive the
shot map. Legend for all maps (portrait, player at bottom, coordinates per
PLAN.md §5.3):

```
F flipper   S slingshot   I inlane   O outlane   P pop bumper
T standup   D drop target R rollover K kicker    C captive ball
M magnet    G one-way gate  ~ ramp entrance  || lane walls
^ plunger lane   * light insert   ( ) orbit lane mouth
```

#### Skeleton A — "classic fan"

A fan of 5 shots spread evenly across both flippers. The safest skeleton;
best when the signature mechanic lives in rules, not geometry.

```
+----------------------------+  y=1.04
|  R R R        top_lanes_n  |
|   P P P       pop_cluster  |
| (A                    B) ^ |
| ||  ~L   [D][D][D]  ~R || ||
| ||  ||      K       || || ||
| ||  ||   T     T    || |G ||
|  \\ ||              || |__/|
|   \\||              ||/    |
|    O I   * * *    I O      |
|    |S\            /S|      |
|    |  \F        F/  |      |
|           drain            |  y=0
+----------------------------+
x=0                      x=0.52
```

Prefabs: `flipper_pair_standard`, `plunger_lane`, `sling_pair`,
`inlane_outlane_pair`, `orbit` (full, both mouths), `ramp_standard` ×2,
`drop_bank_n` (count 3), `pop_cluster`, `top_lanes_n` (count 3). Plus:
1 `kicker` behind the bank, 2 `standup_target`.

#### Skeleton B — "orbit flow"

Loops and combos; the ball keeps moving. Best for speed/momentum themes.
Orbits, inner loop, and ramps all return to an inlane so combos chain.

```
+----------------------------+
|      R R      top_lanes_n  |
| (A================B)     ^ |
| ||  /~U~~\   /inner\  || | |
| ||  |ramp|   | loop |sp|| ||
| ||  | ~L |   (  )---/ || ||
| ||  \____/    spinner || ||
| ||     T  * *  T      |G ||
|  \\               //  |__/|
|   O I   * * *   I O       |
|   |S\           /S|       |
|   |  \F       F/  |       |
|          drain             |
+----------------------------+
```

Prefabs: `flipper_pair_standard`, `plunger_lane`, `sling_pair`,
`inlane_outlane_pair`, `orbit`, `inner_loop` (with `spinner` at its mouth),
`ramp_standard` ×2, `top_lanes_n` (count 2). Plus: 2 `standup_target`,
1 `gate` making the right orbit one-way during multiball.

#### Skeleton C — "widebody chaos"

Wide play area (override `playfield.size` to 0.60 m × 1.04 m per
09-table-format.md), nudging-heavy, mid-field bounce; for chaotic themes
and magnet/kicker signatures. The left-outlane kickback is mandatory — a
widebody without one is a drain-o-matic (§9).

```
+---------------------------------+
|  R R R R          top_lanes_n   |
|    P  P  P  P     pop_cluster   |
| (A                          B)^ |
| ||    M         M           |||||
| ||  T   [D][D]    T         |||||
| ||    K        K  kickers   ||G||
| ||       C  captive_ball    ||_/|
|   O  I    * * * *      I  O    |
|   |k  S\              /S  |    |  k = outlane kickback
|   |kick |  \F      F/  |  |    |
|             drain               |
+---------------------------------+
```

Prefabs: `flipper_pair_standard`, `plunger_lane`, `sling_pair`,
`inlane_outlane_pair`, `orbit`, `pop_cluster` (always 3 pops),
`drop_bank_n` (count 2), `top_lanes_n` (count 4). Plus: 1 `pop_bumper`
(the fourth pop), 2 `magnet`, 2 `kicker`, 1 `captive_ball`, 2
`standup_target`, 1 kickback (`kicker` in the left outlane).

#### Skeleton D — "mini-field tech"

An upper mini-playfield on `layer: 1` with its own small flipper is the
centerpiece; the lower field exists to earn visits upstairs. Best for
build/craft/venue themes. The lower field has 4 shots, not 6 — the
mini-field is the fifth and sixth.

```
+----------------------------+
| +------------------+       |
| | layer 1 minifield|  ~U   |
| |  T T T    K(exit)|  up | |
| |   f  (mini flip) |  ramp |
| +--------||--------+  || ^ |
| (A  down-||-ramp      || |||
| ||  ~L   ||  [D][D]   || |G|
| ||  ||   ||    C      || |||
|  \\ ||   \\->I        ||_/ |
|   O I    * *      I  O     |
|   |S\             /S|      |
|   |  \F         F/  |      |
|          drain             |
+----------------------------+
```

Prefabs: `flipper_pair_standard`, `plunger_lane`, `sling_pair`,
`inlane_outlane_pair`, `orbit` (left mouth only), `ramp_standard` ×2 (right
ramp `~U` climbs to layer 1; a down-ramp returns to the left inlane),
`drop_bank_n` (count 2). Plus on layer 1: 1 short `flipper` (bat length 0.055 m per
09-table-format.md flipper params), 3 `standup_target`, 1 `kicker` exit.
Plus lower: 1 `captive_ball`.

### 4.2 The shot map method

A **shot** is a deliberate, named, repeatable path from a flipper to an
element. Elements not on a shot line are furniture. Method:

1. For each flipper compute the **sweet spot**: the point 70 % along the bat
   from the pivot, at the rest angle. (Flipper geometry comes from
   `flipper_pair_standard`; see 09-table-format.md.)
2. From each sweet spot, draw straight **shot lines** to every element you
   intend to be shootable; a shot line needs 13.5 mm (ball radius)
   clearance from every wall and post on each side.
3. Measure each shot's angle θ **from the +y axis** (straight up-table = 0°):

   ```
   θ = atan2(|tx − sx|, ty − sy) · 180/π
       (sx,sy) = sweet spot, (tx,ty) = target center
   ```

4. **Every major element must lie on a shot line with 15° ≤ θ ≤ 75°.**
   Below 15° the shot sits over the drain gap and rejects come back
   straight down the middle; above 75° a flipper cannot deliver useful
   speed. If your signature element violates this, move the element.
   The 15° floor is about the drain gap, so it binds only shots taken from
   a layer-0 flipper. A layer-1 mini-flipper has no drain beneath its
   reject path (the layer's gravity chute catches it, §4.3), so its shots
   are exempt from the floor — the 75° ceiling still binds.
5. Label every shot in `design.md` §Shots:

   | Shot id | Element(s) | From flipper | θ (deg) | Difficulty 1–5 |
   |---------|-----------|--------------|---------|----------------|

6. Difficulty is computed, not vibed. Angular tolerance
   `α = atan((w/2 − r_ball) / d)` where `w` = clear opening width of the
   shot mouth, `d` = distance from sweet spot to mouth, `r_ball` = 0.0135 m:

   | Difficulty | α |
   |-----------|---|
   | 1 (gimme) | ≥ 4.0° |
   | 2 (easy) | 3.0–4.0° |
   | 3 (standard) | 2.0–3.0° |
   | 4 (hard) | 1.2–2.0° |
   | 5 (expert) | < 1.2° |

Shot count and spread rules: 5–8 labeled shots total (mini-field shots
count); each flipper owns 3–5; at least one shot is backhandable (reachable
from the same-side flipper at θ ≤ 30°); at most one difficulty-5 shot, and
it may gate only bonus value, never mode start (the wizard may require it
at most once); the spread must include at least one difficulty 1–2 and at
least two 3s.

### 4.3 Geometry rules

Normative numeric limits (lane widths, wall thickness, element clearances,
prefab internals) live in the numbers table of 09-table-format.md; the
validator enforces them. Authoring-level rules on top of them:

- Never override prefab-internal geometry (flipper gap, sling shape, inlane
  rail) by more than ±4 mm from the prefab default; feel constants in
  08-physics.md are tuned against those shapes.
- Any wall-to-wall gap between 6 mm and 34 mm is a ball trap or ball wedge
  (ball Ø 27 mm) — close it below 6 mm or open it above 34 mm. `tb_validate`
  catches the passable-but-tight range as V006 (§8.1).
- Nothing but art may occupy the **flipper reaction zone**: the rectangle
  from the flipper baseline up 0.12 m across the full width between the
  slings. A post or target there causes uncatchable rebounds.
- Ramps are 1-D paths with a height profile (PLAN.md §5.6): entrance on a
  shot line, monotonic climb (max grade per 09-table-format.md), exit
  feeding an inlane, habitrail return, or layer-1 area — never an exit
  dropping the ball above the outlanes.
- Upper playfields are `layer: 1` areas; layers connect only via ramps and
  kicker/VUK exits. Every layer-1 area needs a drain path back to layer 0
  that does not depend on script action.

### 4.4 Risk/reward placement patterns

Place value where danger is; the player's risk decisions are the game.

- **Danger near reward:** the highest-value repeatable shot must border a
  risk: a magnet, an exposed drain angle on rejection, or a sling feed. A
  high-value shot with a safe reject teaches lazy play.
- **Safe shots score less:** any θ ≤ 25° backhand or ramp whose reject
  falls back to the same flipper is capped at "minor shot" value (§5.1).
- **Outlane risk vs kickback:** outlanes must claim 15–30 % of drains
  (§8.3). Give exactly one outlane a conditional rescue (kickback, gate, or
  ball-save light) that rules can arm; never both outlanes at once.
- **The sling triangle:** slings punish slow central dribbles — their job —
  but sling power more than 10 % above the 08-physics.md default turns them
  into drain pumps (§9).

## 5. Rules design

### 5.1 The scoring economy template

Fill this table in `design.md` §Economy before writing full rules. All
values in points; ranges are hard bounds. They bound the **base** value
written in rules — before playfield multipliers, per-order/per-level
scaling, and combo-length scaling.

| Event class | Value range | Examples |
|-------------|------------|----------|
| Switch | 1,000 – 10,000 | rollover, sling, pop, spinner tick |
| Minor shot | 10,000 – 75,000 | standup, unlit ramp, orbit, letter award |
| Major shot | 50,000 – 300,000 | lit ramp, bank complete, loop combo |
| Mode award | 250,000 – 2,500,000 | mode shot made, mode completion bonus |
| Jackpot | 300,000 – 1,000,000 | multiball jackpot |
| Super jackpot | ≤ 3,000,000 | the multiball's one big payoff: **≤ 3× the 1,000,000 jackpot cap** |
| Wizard award | 5,000,000 – 15,000,000 | wizard completion, one per game typically |

The super row is a cap, not a band: a super may be as small as an ordinary
jackpot, but never above 3× the jackpot cap. All five shipped supers sit
inside it (atomic-diner 2,000,000; voltage-vandals 2,500,000; neon-drift,
tilt-o-tron and cosmic-carnival 3,000,000), so a super needs no §11 item 4
justification line.

These are the bands the five shipped tables are written in
(15-launch-tables.md: switch 4,000–5,000; ramp made 50,000–60,000; bank
complete 200,000–250,000; mode completion 1,500,000–2,500,000; multiball
jackpot 400,000–600,000; super jackpot 2,000,000–3,000,000; wizard payoff
8,000,000–12,000,000). A table's
`meta.autoplay_bounds` may **narrow** the metric ranges these bands produce
(§8.3), never widen or contradict them (09-table-format.md §2).

**Expected game score (EGS):** the skill-2 autoplay median of a 3-ball game
(`score.p50`, `--balls 3 --skill 2`). Read it at **`--runs 20`** while you
iterate (§2 step 11) and at **`--runs 500`** for the binding acceptance
number (15-launch-tables.md §0.7); quote the run count alongside the value
in `design.md`, because a 20-run median moves several percent between seeds
and a 500-run one does not. Target band: **3,000,000 – 14,000,000**. The
whole economy is tuned until EGS lands in the band (§8.4).

**The 15 % balance rule:** for every individually repeatable shot,
`value × expected_makes_per_game(skill 2) ≤ 0.15 × EGS`. The autoplay report
gives per-shot makes; multiply and check. Any violator gets a decaying value
(e.g. 100 % → 50 % → 25 % on consecutive makes, resetting each ball) or a
lit/unlit gate.

End-of-ball bonus (`tb.add_bonus`, `tb.set_multiplier`) must total 3–15 %
of EGS across a game (the five shipped tables sit at 3–6 %), multiplier
capped at 6×; bonus is lost on tilt only (tilt handling per
11-game-framework.md).

### 5.2 Mode architecture patterns

Pick one primary pattern; the signature mechanic decides which.

| Pattern | Shape | Use when |
|---------|-------|----------|
| Ladder | Modes in fixed order, each harder, wizard at the top | Signature is a build/progression |
| Parallel-select | 3–6 modes, player chooses at a start shot (lane-change to browse), wizard when all done | Signature is variety/decision-making |
| Hurry-up | Value counts down from a max in real time; shot collects current value | Signature is speed/pressure |

Rules that apply to all patterns:

- 3–6 modes. Fewer is thin; more is mode spam (§9).
- A mode is: a start condition, 1–3 named shots, a timer (§5.4), an award
  (§5.1 "mode award"), and a light + music statement (§6, §7). If you
  cannot state all five in one line each, the mode is not designed yet.
- **Stacking defaults (override deliberately):** one timed mode at a time;
  multiball may stack onto a running mode (mode timer pauses); two
  multiballs never stack; the wizard stacks with nothing.
- Mode progress persists across balls; mode *timers* do not (a drained ball
  ends the timed mode without award).

### 5.3 Multiball design

Every table ships at least one multiball. Design it as a loop, not a shower:

```
lock sequence (1–3 balls via ball_lock or scripted kicker holds)
  → multiball_start (tb.add_ball to 2–4 balls total)
  → jackpot shot lit (one named shot, value from §5.1)
  → jackpot collected → jackpot moves or super-jackpot sequence lights
  → super collected → loop relights at +25 % value, cap 2× base
  → last ball standing → multiball_end, restart available for 10 s
```

Balls in play: 2–3 standard; 4 only as the table's stated spectacle
(PLAN.md §5.8 Tilt-O-Tron). Add-a-ball at most once per multiball. Switch
scoring may double during multiball but must return to base at
`multiball_end` — permanent inflation breaks the economy.

### 5.4 Pacing knobs

| Knob | Default | Range | Effect |
|------|---------|-------|--------|
| Ball save (`tb.ball_save`) | 8 s | 5 – 15 s | Longer = friendlier, shorter EGS spread |
| Mode timer | 30 s | 20 – 45 s | Shorter = pressure; must exceed 2× median shot interval |
| Hurry-up decay | max→min over 12 s | 8 – 20 s | Countdown pressure |
| Multiball restart window | 10 s | 0 – 15 s | Forgiveness after quick drain-out |
| Extra ball light | at 40 % of EGS | 25 – 60 % | One per game maximum |
| Kickback rearm | complete inlane rollovers | any earned condition | Never free after first use |

Difficulty curve across balls: ball 1 is friendliest (skill shot + full
ball save); ball 3 needs a comeback lever (e.g. mode awards +50 %). Check
the report's `per_ball_p50` (§8.2): if ball 3's median is < 60 % of
ball 1's, add the lever.

## 6. Art pass method

Style rules, palettes, TBArt schema, and the theming worksheet are owned by
13-art-direction.md. The authoring method is three sub-passes:

1. **Blockout:** flat fills only — playfield ground color, lane guides,
   element footprints in palette mid-tones. Goal: geometry reads as shapes.
2. **Theme:** decals, iconography from the worksheet motifs, art per zone.
   Every named shot gets a visual arrow/lane treatment pointing along its
   shot line.
3. **Glow polish:** light inserts, bloom accents, particles. Glow budget: at
   most 15 % of playfield area emissive at once in the default (non-mode)
   state; validator warns above it.

Readability checklist (each item verified on `tb_screenshot` output):

- [ ] Every lane and shot mouth visible in the `full` view at 1080p portrait.
- [ ] Ball-vs-background contrast: no travel path where the ball's sphere
      art blends into ground art (13-art-direction.md contrast table).
- [ ] Each light color means exactly one thing table-wide (e.g. white =
      shoot this now, theme-accent = mode progress, red = danger/tilt) and
      the meaning table is written in `design.md` §Theme.
- [ ] Lower third is not visually busier than the mid-field: the player's
      eyes live there during flips.
- [ ] Attract state (lights cycling, no ball) reads as inviting, not as a
      wall of bloom.

## 7. Audio pass method

Formats, synth patch parameters, tracker patterns, and the music state
machine are owned by 12-audio.md. Method:

1. **Event mapping:** list every event your rules emit (PLAN.md §5.7 list
   plus custom mode events) in `design.md` §Audio; map each to a patch in
   `audio.json`. Every scoring event must have a sound — the ship checklist
   (§11) fails on any unmapped scoring event (warning V038, §8.1).
2. **Hierarchy:** louder/brighter = rarer. Switches get short quiet blips;
   major shots get the theme motif; jackpots and wizard get the biggest
   patches. If everything is loud, nothing is.
3. **Main loop:** compose the main-play music loop, minimum 16 bars, in the
   worksheet's music adjectives, using the tracker format of 12-audio.md.
4. **Music state plan:** cover every 12-audio.md music state (attract, main
   play, mode, multiball, wizard, game over at minimum) with a distinct
   pattern set or an explicit reuse noted in `design.md` §Audio. Mode music
   outranks main play in energy; wizard is the peak.

## 8. Tooling reference

### 8.1 tb_validate

```
tb_validate tables/<slug> [--json] [--strict]
```

Validates all four pack files plus cross-references between them. The CLI,
the **V-code** catalog, the diagnostic line format, and the exit codes are
owned by 09-table-format.md (§8 catalog, §10 output): every finding line is
`file:pointer [Vnnn][severity] message`; exit `0` = no errors (warnings
allowed), `2` = errors present (`--strict` promotes warnings to failures,
as CI does), `3` = file/IO failure. Severity is fixed per code by the 09
catalog. What this guide adds is the authoring response per code group:

| Codes (09 §8) | Domain | You fix it by |
|---------------|--------|---------------|
| V000, V022, V026 | JSON syntax, format version, unknown keys | Fixing the JSON to the 09 schema; never suppressing |
| V001, V002, V017, V018, V023, V024, V025, V027 | Ids, references, types, cardinality | Fixing names in the offending file against `table.json` ids and the 09 schema |
| V009, V012, V019, V020, V021 | Element parameter ranges, materials | Clamping the parameter to the 09 numbers tables |
| V003–V006, V008, V013–V016 | Geometry (boundary, out-of-bounds, tight gaps/lane widths, flipper gap, perf caps) | Moving/resizing geometry per §4.3 |
| V007, V010, V011 | Reachability (flood fill from plunger), ramp continuity, layer connections | Re-routing lanes/ramps until the ball can reach and leave everything |
| V032, V033 | Rules lint (switch never scored, light never driven) | Wiring the element into rules, or deleting it |
| V034–V036 | Art (unknown palette/layer refs, glow budget, contrast) | Fixing `art.json` per 13-art-direction.md |
| V037–V039 | Audio (missing patch refs, unmapped scoring events, music state gaps) | Fixing `audio.json` per §7 |

V032–V039 are the authoring-loop checks this workflow relies on; they are
catalogued normatively in 09-table-format.md §8 alongside V000–V031 and
land at M15: V032 scoring element never scored by rules (warning); V033
light never driven by rules (warning); V034 unresolved `art.json`
palette/layer/light/decal reference (error); V035 glow budget exceeded
(warning, §6); V036 ball-path contrast below the 13-art-direction.md table
(warning); V037 missing synth patch reference (error); V038 scoring event
with no mapped sound (warning, §7); V039 music state without a song
(warning, §7). Codes V028–V031 are *not* these checks — 09 §8 assigns them
to `meta.default_scores`, `meta.autoplay_bounds`, `light_groups`, and the
light `function` field.

Fix the **first** geometry error before later ones; geometry errors cascade
(one out-of-bounds wall can generate dozens of reachability findings).

### 8.2 tb_autoplay

```
tb_autoplay tables/<slug> --runs N --skill {0|1|2} --seed S
             [--balls 3 | --seconds 300] [--report out.json]
             [--check-bounds] [--replay tape.replay.json]
             [--record-golden out.hashes]
```

This section is the **normative `tb_autoplay` contract** — CLI flags,
skill profiles, and report schema. Invocations elsewhere (04-milestones.md
M15, 15-launch-tables.md §0.7, 16-testing-ci.md §2.8/§3) conform to it;
flags may be extended here, never changed.

Headless deterministic simulation (same `tb_sim` core, PLAN.md §5.3): each
of the N runs (`--runs`, default 1) seeds the RNG with `S + run_index`,
performs a scripted plunge, and plays with a **flipper policy**. Each
flipper has a *flip window* — the region swept by its bat extended 60 mm
up-table; when the ball center enters a window moving down-table
(v_y < 0), the policy schedules a flip aimed at a chosen shot, with
skill-dependent delay and error, nudging within tilt-warning limits at
higher skills.

**Session shapes (exactly two; run both).**

| Shape | What it is | Fields it produces |
|-------|-----------|--------------------|
| `--balls 3` | N complete games, one per run, each ending at game over | every report field, including the game-scoped ones |
| `--seconds 300` | one continuous session, instant respawn on drain, no game boundaries | every field except the game-scoped ones, which are emitted as `null` |

Game-scoped fields are `score.*` (incl. `per_ball_p50`), `modes.*`,
`tilt_warnings_per_game`, and `ball_saves_used_per_game` — the four that
need a game boundary to mean anything. Everything else
(`ball_time_s.*`, `drains.*`, `coverage.*`, `shots[]`, `flips_per_ball`,
`script_errors`, `stuck_balls`, `tilts`) is produced by both shapes;
§8.3 says which shape each metric is **read** from, and CI gates the two
shapes in two separate jobs (16-testing-ci.md §2.8 runs only the
`--seconds 300` smoke).

CI/regression flags: `--replay <tape>` plays a recorded input tape
(16-testing-ci.md §2.4.2 format) instead of the flipper policy;
`--record-golden <path>` (requires `--replay`) writes the determinism
state-hash golden file of 16-testing-ci.md §2.4. `--check-bounds` makes
the exit code a verdict: pass only with zero error-severity log lines,
`stuck_balls` = 0, and every **applicable** measured metric inside the
table's declared bounds (declared in `table.json`
`meta.autoplay_bounds` per 09-table-format.md §2, values per
15-launch-tables.md §x.7). A declared bound is applicable to this run when
the run's `--skill` equals the bound's recorded skill **and** the run's
session shape is the shape §8.3 assigns to that metric; bounds that are
not applicable are skipped silently, and a metric that is `null` for this
shape is never a violation. Exit codes: `0` ran to completion (with
`--check-bounds`: and all applicable checks green); `1` sim/script error,
stuck ball, or bounds violation; `2` usage/IO error.

**Stuck detection** (normative for 16-testing-ci.md §2.8): a ball is
stuck when its center stays within one ball diameter (0.027 m) of a fixed
point for 10 s of sim time while not held by a kicker, ball lock, flipper
cradle, or scripted magnet hold. Each occurrence increments the report's
`stuck_balls` and the run continues by respawning the ball.

| Skill | Reaction delay | Timing σ | Aiming | Nudge |
|-------|---------------|----------|--------|-------|
| 0 | 120 ms | 60 ms | none — flips whenever ball in window | never |
| 1 | 80 ms | 25 ms | random labeled shot, retried | on sling rattle |
| 2 | 50 ms | 10 ms | highest-value currently-lit shot; backhands; dead-flips to catch | yes, incl. outlane slap saves |

Report fields (one JSON object; aggregate over runs):

```json
{
  "table": "atomic-diner", "runs": 20, "skill": 2, "seed": 2002,
  "shape": "balls3", "balls": 3,
  "script_errors": 0, "stuck_balls": 0,
  "ball_time_s":  {"p10": 18.2, "p50": 44.6, "p90": 96.4},
  "drains": {"center": 0.26, "left_outlane": 0.11,
             "right_outlane": 0.09, "outlane_share": 0.20},
  "coverage": {"hit": 17, "total": 19, "share": 0.89,
               "missed": ["shaker", "ger_targets"]},
  "shots": [ {"id": "counter_ramp", "attempts": 214, "made": 45, "rate": 0.21},
             {"id": "order_window", "attempts": 186, "made": 48, "rate": 0.26} ],
  "score": {"p10": 2.6e6, "p50": 6.0e6, "p90": 9.4e6,
            "per_ball_p50": [2.3e6, 1.8e6, 1.9e6]},
  "modes": {"started_per_game": 2.4, "completed_per_game": 1.1,
            "multiball_reach_share": 0.38, "wizard_reach_share": 0.04},
  "flips_per_ball": 41, "tilt_warnings_per_game": 0.6, "tilts": 0,
  "ball_saves_used_per_game": 0.8
}
```

`shape` is `"balls3"` or `"seconds300"`; the same object shape is emitted
for both, with the game-scoped fields `null` in a `"seconds300"` report.
`coverage.total` counts every scoring element (everything that can emit a
scoring event; lights and pure walls excluded). `shots[<id>].rate` is the
metric path 09-table-format.md §2 accepts for a per-shot bound
(`made ÷ attempts` of the `shots[]` entry with that `id`).

### 8.3 Metric target ranges

A metric is **green** when inside its range. "Layout rows" are checked from
step 6 onward; the rest from step 11.

**This table is the per-metric authority.** It fixes, for every metric, the
skill it is read at, the session shape it is read from, and whether it can
be declared as a `meta.autoplay_bounds` bound. Every other document
(15-launch-tables.md §0.7 and §x.7, 16-testing-ci.md §2.8, and each table's
`meta.autoplay_bounds`) reads its skill and shape from here and never
reassigns them. **Every declared bound records the skill it is measured at**
(`{min, max, skill}`, 09-table-format.md §2); its shape is the shape this
table assigns to that metric path.

| Metric | Target | Skill | Session shape | Bound? | Phase |
|--------|--------|-------|---------------|--------|-------|
| `ball_time_s.p50` | 22 – 60 s | 1 | 300 s | yes | layout |
| `drains.center` | < 0.35 | 0, 1, 2 | 300 s | yes (declare at skill 1) | layout |
| `drains.outlane_share` | 0.15 – 0.30 | 1 | 300 s | yes | layout |
| `coverage.share` | ≥ 0.80 | 1 | 300 s | yes | layout |
| `stuck_balls` | 0 | all | 300 s | yes (declare at skill 1) | layout |
| `script_errors` | 0 | all | both | no (exit code 1 already fails) | rules |
| `tilts` | 0 (policy must not tilt) | 2 | both | yes (declare at skill 2) | layout |
| `shots[<id>].rate`, difficulty 1–2 shots | ≥ 0.40 | 2 | balls 3 | yes | layout |
| `shots[<id>].rate`, every labeled shot | ≥ 0.10 (≥ 0.08 mini/upper-flipper-only, see below) | 2 | balls 3 | yes | layout |
| `shots[<id>].rate`, every labeled shot | ≥ 0.02 | 0 | balls 3 | yes | layout |
| `score.p50` (EGS) | 3.0e6 – 14.0e6 | 2 | balls 3 | yes | rules |
| `modes.started_per_game` | 1.5 – 4.0 | 1 | balls 3 | yes | rules |
| `modes.multiball_reach_share` | 0.10 – 0.60 | 1 | balls 3 | yes | rules |
| `modes.wizard_reach_share` | 0.005 – 0.10 | 2 | balls 3 | yes | rules |
| `score.p50` skill spread | skill-0 p50 < 0.25 × skill-2 p50 | 0 vs 2 | balls 3 | no (cross-run) | rules |
| `score.p90 ÷ score.p10` spread ratio | per table (15-launch-tables.md §x.7) | 2 | balls 3 | no (review-only) | rules |
| 15 % rule (§5.1) per shot | ≤ 0.15 × EGS | 2 | balls 3 | no (hand-computed) | rules |

How the two shapes are gated in CI (16-testing-ci.md §2.8):

- The **300 s smoke** (`--skill 1 --seconds 300 --check-bounds`, one job per
  table, every PR) gates the session-shape-independent rows:
  `ball_time_s.p50`, `drains.*`, `coverage.share`, `stuck_balls`.
- A separate **`--balls 3` job** gates the game-scoped rows: `score.p50`,
  `modes.*`, and every `shots[<id>].rate` bound, each at the skill this
  table assigns it (so that job runs the skills it needs — 0 and 2 for shot
  rates, 1 for `modes.started_per_game`/`modes.multiball_reach_share`, 2 for
  `score.p50`/`modes.wizard_reach_share`).
- Rows marked "no" are never mirrored into `meta.autoplay_bounds`: cross-run
  ratios need two reports, and the 15 % rule is arithmetic over the report,
  not a report field. They are checked by the author at step 11 and recorded
  in `design.md` §Ship.

A table declares its own numbers inside these ranges — a
`meta.autoplay_bounds` entry always **narrows** the row above it and may
never widen or contradict it (e.g. Atomic Diner's `score.p50` 4.5e6–7.5e6
and `modes.wizard_reach_share` 0.02–0.06 both sit inside their rows).

**The one carved-out exception (mini/upper-flipper shots).** A shot that is
reachable *only* from a mini or upper flipper — not makeable from either
main flipper at all — may declare a floor as low as **0.08** on the skill-2
`shots[<id>].rate` row instead of 0.10. Such a shot is fed by a flipper the
policy reaches only after an upstairs trip, so its attempts are few and its
rate is structurally below a main-flipper shot's. The row above therefore
reads ≥ 0.10 for a main-flipper shot and ≥ 0.08 for a mini/upper-flipper-only
one: declaring 0.08 on such a shot **conforms** to the row, it does not widen
it, and the 0.10 floor stands untouched for every other shot. Declare the
exception on that one shot id, and say in `design.md` §Shots
which flipper is the sole feeder (cosmic-carnival: `shots[tent].rate`
0.08–0.16, mini-flipper attempts only, 15-launch-tables.md §4.7). The
exception applies to this row only — the skill-0 floor (≥ 0.02) and the
difficulty 1–2 floor (≥ 0.40) are declared as written.

### 8.4 The tuning matrix

For each out-of-range metric, apply fixes **in the listed order**, one at
a time, re-running autoplay (same seeds, same shape) after each; stop at the
first fix that brings the metric in range without pushing another out. "Band"
below means the table's own `meta.autoplay_bounds` band when it declares one,
otherwise the §8.3 row (generic limits quoted in parentheses).

**Precedence against 15 §0.7.** For the five tables shipped in
15-launch-tables.md, **15 §0.7's ordered five-step list is the first resort**
when a target misses: it is written against the single §0.4 standard bottom
those five share, and it carries a `slope` step this matrix has no row for.
This matrix is the general authoring tool — it covers every metric and every
fix those five steps do not reach (any new table, the score / mode /
coverage / shot-rate rows, and layout fixes such as §10's re-angled
`ger_targets` face), and it is where you go once that list is exhausted or
when the missed metric is not one it moves. One difference stands: 15 §0.7's
steps 1 and 3 (outlane gap, sling `kick_speed`) may change a §0.4 shared
value and record it as a per-table roster override; a §8.4 step never
invents one — if a step would need a §0.4 value the table does not already
override, walk on down the row (§10).

| Out-of-range metric | Ordered fixes |
|---------------------|---------------|
| Ball time p50 below band (generic floor 22 s) | 1. Soften sling impulse 10 % (08-physics.md sling param). 2. Narrow each outlane 4 mm. 3. Add/extend inlane rail so sling exits feed flippers. 4. Re-angle any element face that rejects toward the drain (rotate 4–6° outward). 5. Lengthen ball save to 10–12 s (last resort — masks, not fixes). |
| Ball time p50 above band (generic ceiling 60 s) | 1. Strengthen slings 10 %. 2. Widen outlanes 4 mm. 3. Remove one safe repeatable catch (e.g. make a backhand ramp reject occasionally via gate). 4. Raise pop bumper impulse 10 %. |
| Center drain above band (generic ceiling 0.35) | 1. Find the feeding element: check which shot's rejects precede center drains in the report's drain-context list; rotate that face 4–6°. 2. Soften slings 10 %. 3. Add a center post ONLY if the table's identity wants one (it changes feel table-wide); prefer 1–2. |
| Outlane share > 0.30 | 1. Narrow outlanes 4 mm. 2. Raise outlane divider top by 10 mm (catches earlier). 3. Arm the kickback earlier in rules. 4. Reduce sling impulse 10 %. |
| Outlane share < 0.15 | 1. Widen outlanes 4 mm. 2. Lower divider 10 mm. 3. Make kickback earned, not default-armed. |
| Coverage share < 0.80 | For each element in `coverage.missed`: 1. Put it on a labeled shot line (§4.2) — most misses are furniture. 2. Widen its approach lane up to +6 mm. 3. Reduce its difficulty (move 20–40 mm closer to a sweet-spot line). 4. If it is layer-1: check the feeding ramp's make rate first. |
| Shot rate < floor (per §8.3) | 1. Recompute α; widen mouth to the next difficulty band. 2. Clear post/wall clipping the shot line. 3. For layer-1 shots, ease the feeding ramp grade. |
| EGS below band (generic floor 3.0e6) | 1. Raise mode awards toward their §5.1 caps. 2. Increase jackpot base. 3. Add spinner/pop value. Do NOT lengthen ball time to fix score. |
| EGS above band (generic ceiling 14.0e6) | 1. Find 15 %-rule violators, apply decaying value. 2. Lower jackpot/mode awards. 3. Cap multiball switch doubling duration. |
| 15 % rule violated | 1. Decaying repeat value (§5.1). 2. Lit/unlit gating so rules meter the shot. 3. Lower base value. |
| Modes started < 1.5 | 1. Ease mode-start condition (fewer qualifying shots). 2. Move mode-start to an easier shot (difficulty −1). |
| Modes started > 4.0 | 1. Raise qualifying requirement. 2. Add a cooldown (one mode start per ball segment). |
| Wizard reach below band (generic floor 0.005) | 1. Check the table's own band first — a rare wizard is a design choice, and the shipped tables deliberately target 0.005–0.06 (15-launch-tables.md §x.7); act only when the table's own floor is missed. 2. Ease the hardest *gating* mode (difficulty −1 on its shots) with the requirement list intact. 3. Only then drop one qualification step (e.g. 3 of 4 words) — it changes the table's identity, so record it as a design decision in `design.md`. |
| Wizard reach above band (generic ceiling 0.10) | 1. Add one requirement (e.g. + one multiball jackpot). 2. Raise the hardest gating mode's completion bar. |
| Skill spread violated (skill 0 ≥ 25 %) | 1. Shift value from switches/pops (skill-agnostic) into aimed shots. 2. Do NOT shorten the ball save to fix it (that moves ball-time metrics instead); reduce passive scoring *during* the ball save. |

### 8.5 tb_screenshot

```
tb_screenshot tables/<slug> --views full,lower,upper,backglass,attract
              [--state <mode-id|multiball|wizard>] --out review/
```

Renders the table headlessly to PNGs: `full` (whole playfield, portrait),
`lower` (bottom third), `upper` (top third or layer-1), `backglass`,
`attract` (attract light cycle, 3 frames); `--state` renders a given mode's
light state. Review every image against this checklist; findings go to
`design.md` §Ship:

- **Composition:** does the eye find the signature mechanic first? Is the
  visual weight where the shots are, not in empty corners?
- **Readability:** can you name every shot mouth from the `full` view
  alone? Is any lane entrance camouflaged by art?
- **Glow overload:** in `attract` and `--state multiball`, is bloom pooling
  into unreadable white? (Glow budget §6.)
- **Dead zones:** any region with neither an element nor art interest?
  Dead visual zones usually mark dead gameplay zones (§9).
- **Ball contrast:** in `lower`, would a 27 mm silver ball vanish anywhere?

## 9. Common failure smells

| Smell | Symptom in metrics/screenshots | Diagnosis | Fix |
|-------|-------------------------------|-----------|-----|
| Dead lower third | Low flips_per_ball, short ball times, `lower` screenshot empty of targets/lights | Nothing to do below mid-field; ball transits it only to drain | Move a standup pair or lights into sling-adjacent positions; add inlane-fed skill loop |
| Unreachable toy | Element in `coverage.missed` every run | Toy off all shot lines, or approach lane clipped | §8.4 coverage fixes; toys must sit ON a shot line, not behind one |
| Magnet frustration | Ball time fine but center drains cluster right after magnet events | Magnet releases ball at uncontrolled angle toward drain | Magnet release must throw up-table or to a flipper (08-physics.md release vector); pulse, don't hold-and-drop, below y = 0.4 m |
| Mode spam | modes.started_per_game > 4, mode music never finishes a loop | Start condition too cheap; modes overlap in player memory | Raise qualification; enforce one-at-a-time stacking (§5.2) |
| Drain-o-matic slings | Outlane share > 0.35 with sling hits preceding | Sling power over default, or sling-outlane geometry funnels | Reduce sling impulse to default; check divider height (§8.4) |
| Invisible rules | Score rises but modes.completed low at skill 2; screenshots show no lit path | Rules never tell the player what to shoot | Every active objective needs exactly one blinking light on its shot (10-scripting.md `tb.light_blink`) plus a `tb.show_message` on state change |

## 10. Worked example: Atomic Diner (abbreviated)

This is the **actual shipped design** of 15-launch-tables.md §2, compressed
into this guide's twelve artifacts — not a variant. Every name, coordinate,
and award below is 15 §2's; if the two ever disagree, 15 wins and this
section is corrected in the same PR. Atomic Diner is built at M16 by walking
§1 top to bottom (15 §2.8), which is what makes it the pipeline dogfood.

**1. Concept (3 sentences).** A chrome-and-cherry googie diner floating in
space where the player works the room as short-order cook. The one new
thing: you *spell* food (BURGER, FRIES, SHAKE) across the field and then
assemble the collected words into orders delivered at the order window —
the pace is set by collection, not by timers. Wizard fantasy: DINNER RUSH,
every order back at once with the kitchen in flames.

**2. Worksheet (extract).** Palette: 13-art-direction.md `atomic-teal` (the
binding assignment for atomic-diner per 13 §2.2): sea-teal ground `#06181D`,
neon teal `#1BE7D2`, cherry-coral `#F45D48`, lime/mustard accents,
`glow_white` `#EAFFF8` only as halo cores. Motifs: (1) starburst/atomic
orbital clocks with electron dots, (2) boomerang countertop trim along the
lanes, (3) neon script tubing framing the scoops and the counter.
Vocabulary: shots are diner furniture (COUNTER ramp, SHAKER captive ball,
ORDER WINDOW scoop, F-R-I lanes, B-U-R / G-E-R standups, P-I-E on the
counter); modes are "orders"; the counter multiplier is "tips". Music:
"greasy + optimistic" space-age bachelor-pad swing.

**3. Identity.**

```
signature : food-word collection → order assembly — three spelled words
            (BURGER / FRIES / SHAKE) are the ingredients an order consumes
support 1 : the counter, an upper mini-playfield on layer 1 with its own
            flipper; P-I-E up there pays and raises the tip multiplier
support 2 : the shaker captive ball — full-travel hits spell SHAKE, then fill
            the blender 1/3 at a time into Milkshake Multiball (seed 6 flavored)
wizard    : DINNER RUSH — Orders 1–3 delivered + Milkshake MB started;
            three phases ending in one 8,000,000 shot to the order window
distinctiveness: delete word collection and the counter, the shaker and the
            order window have nothing to feed — every mode entry disappears
            and the table is a shot gallery. PASS.
```

**4. Layout (15 §2.3 roster).** Skeleton D (mini-field tech), default play
area 0.52 × 1.04, standard bottom of 15 §0.4 (`flipper_pair_standard` pivots
`[0.141, 0.115]` / `[0.339, 0.115]`, `sling_pair`, both
`inlane_outlane_pair`s, `plunger_lane`, outhole + 4-ball trough).

| Prefab / element | Values |
|------------------|--------|
| `orbit` | `mouth_x` 0.075 (the default, and there is no `lane_width`: the table boundary *is* the lane's outer edge, so `mouth_x` is the lane width) ⇒ left lane [0.000, 0.075], guide wall x 0.075, **lane centre x 0.0375**; `entry_y_left` **0.660**, above the `shaker` slot's far end `b` `[0.150, 0.640]` — the captive ball's top reaches y 0.6535, clearing the mouth by **0.0065**, so it never sits in the lane; `entry_y_right` 0.900 (merged with the plunger lane); the apex does **not** feed the F-R-I lanes — the top run is enclosed lane at guide y `H − mouth_x` = 0.965, **0.052** above the bank's 0.913 rubber caps, so a ball round the top exits the *far* mouth (a full plunge rides it and comes out the left mouth at `entry_y_left`) and the bank is entered from below (09-table-format.md §5.5, 15 §0.8/§2.4) |
| `top_lanes_n` "F-R-I" | n 3, centred `[0.300, 0.880]`, `lane_length` 0.050 (lane centres x 0.260 / 0.300 / 0.340), lane change on the flipper buttons |
| `pop_cluster` "soda bubbles" | n 3, centroid `[0.335, 0.770]`, spacing 0.072 (top cap y 0.843, 0.012 under the F-R-I bank — blocked) |
| `counter_floor` + `counter_flipper` | layer-1 walls x 0.055–0.335, y 0.762–1.000; mini flipper `[0.125, 0.815]`, `length` 0.052, `rest_angle_deg` −28, `swing_deg` 48 (the pivot sits 0.023 above the front wall's left segment — at y 0.790 it would be *inside* that segment) |
| `pie_targets` ×3 | `[0.105,0.965]`, `[0.150,0.975]`, `[0.195,0.965]`, `facing_deg` 275, layer 1 |
| `counter_exit` kicker | `[0.290, 0.930]`, saucer, `eject_angle_deg` −80, `eject_speed` 2.4, layer 1 |
| `counter_ramp` / `counter_drop` | ramp `[0.360,0.545]` → arc → `[0.300,0.775]` (up), **no `width` key ⇒ 09 §4.13's 0.044 default**; chute `[0.235,0.672]` → `[0.235,0.762]` (down), length 0.090, grade 0.048/0.090 = **0.533** ≤ V010's 0.60 |
| `counter_hood` | wall `[0.198,0.650]` → `[0.270,0.632]`, `material` "wood" — the chute's deflector, **0.03125** below the layer-0 seam and wider than it on both sides, so a ball coming up-table cannot back-door the chute; its **14.0°** down-right lean (dy 0.018 over dx 0.072 ⇒ 14.0362°) rolls the exiting ball into mid-field. The old dy 0.016 gave **12.5288°**, under the **12.675°** at which `1.11052·sin θ` beats 08 §1.3's 0.24367 m/s² rolling resistance: a moving ball still rolled off (net −0.00277 m/s²) but a stopped one could not restart, against `stuck_balls` 0. At 14.0362° the net is **+0.02499 m/s²** (15 §2.3) |
| `shaker` captive ball | slot `{a:[0.150,0.560], b:[0.150,0.640]}`; struck face at y 0.560; far end `b` is the one whose arrival at ≥ 0.3 m/s emits `captive_full_travel{id}`. **x 0.150, not 0.085:** §0.8 makes `left_orbit` an RF shot on an entry bearing of 117.12°–121°, and every ray in that band crosses x 0.085 between y 0.538 and y 0.611 — through the old slot. At 0.085 the flattest legal ray passed 0.02322 from the resting captive against the 0.027 a ball plus a captive needs (**−0.0038**); at 0.150 it passes **0.03464** (**+0.00764**), and the margin only grows toward 121° (15 §2.3/§2.4) |
| `order_window` kicker | `[0.415, 0.560]`, scoop, `eject_angle_deg` 237, `eject_speed` 3.0 |
| `bur_targets` / `ger_targets` | left wall x 0.070 at y 0.470/0.435/0.400, `facing_deg` 0; centre-right `[0.328,0.416]`, `[0.339,0.384]`, `[0.349,0.352]`, `facing_deg` 195. The G-E-R row is the old `[0.300,0.430]`/`[0.312,0.398]`/`[0.322,0.366]` row translated down-right by **≈ 0.031 m** (+0.027…+0.028 in x, −0.014 in y; the row's steps and its 195° perpendicular are unchanged): at `[0.300,0.430]` the G target sat 0.00127 from the LF→`counter_ramp` ray against the 0.026 a standup face plus a ball needs, permanently blocking this table's hardest shot. Threaded through the 0.035 m channel between that ray (63.010°) and the RF→`order_window` ray (80.308°), G clears the ramp line by **+0.00403** and R the scoop line by **+0.00404** — the two tightest numbers on the table, and what makes both shots below legal (15 §2.3/§2.4) |

Layer bookkeeping: `playfield.layer1_z` = 0.048, and both counter ramps'
final keyframe is `z` 0.048, so each end seams to layer 1 — the exit layer
is *derived* from that z, never authored (09-table-format.md §4.15/§4.21,
V011).

**Shot map (§4.2 arithmetic).** Sweet spots are 70 % along each bat at rest:
LF `(0.1866, 0.0876)`, RF `(0.2934, 0.0876)`, counter flipper
`(0.1571, 0.7979)` (pivot `[0.125, 0.815]` + 0.7 × 0.052 at −28°). θ is the
unsigned sweet-spot chord angle of §4.2;
15 §2.4 lists the same shots by the *ball's direction at the mouth* signed
per 15 §0.3 (+ leans left), so the two columns differ by construction and
both go in `design.md` §Shots. α = atan((w/2 − 0.0135)/d). Every target
point is the element's **mouth**: `left_orbit` is measured to the left lane
centre at the orbit's binding `entry_y_left` — the lane hugs the boundary, so
that centre is `mouth_x`/2, i.e. `(0.0375, 0.660)`, which
gives d 0.627 — measuring to a point further down the lane shortens d and
flatters α, and is the commonest way a shot map lies about difficulty.

| Shot id | From | θ | 15 §2.4 | d (m) | clear mouth w (m) | α | Diff |
|---------|------|---|---------|-------|-------------------|---|------|
| `left_orbit` | RF | 24.1° | +25.8° | 0.627 | 0.075 (orbit lane = `mouth_x`) | 2.19° | 3 |
| `counter_ramp` | LF | 20.8° | −27.0° | 0.489 | 0.044 (**09 §4.13 default** — 15 §2.3 declares no `width`) | 1.00° | **5** |
| `shaker` | RF | 16.9° | +23.0° | 0.494 | 0.068 (slot mouth) | 2.38° | 3 |
| `order_window` | **RF** | 14.4° | −9.7° | 0.488 | 0.064 (scoop mouth) | 2.17° | 3 |
| `bur_targets` | **LF** | 18.6° | +12.5° | 0.366 | 0.075 (bank face) | 3.75° | 2 |
| `ger_targets` | **RF** | 8.7° | 0.0° | 0.300 | 0.070 (bank face) | 4.10° | **1** |
| `pie_targets` | MF | 2.3–17.3° | +7.6 / −8.9 / −25.0° | 0.18 | 0.090 (three faces) | 10.1° | 1 |

The three re-assigned rows — `order_window`, `bur_targets`, `ger_targets` —
take 15 §2.4's flippers, and not as a matter of taste: 15 §0.4's launch
windows are the binding validity test, and this section's earlier column
failed it. `order_window` bears **58.38°** from the LF pivot,
0.62° *flatter* than LF's 59° rest end, so LF cannot put a ball there at all
(from RF: 80.31°). `bur_targets` runs **127.15°–133.35°** from RF, 6.2°–12.4°
past its 121° rest end (from LF: 101.31°–104.00°). `ger_targets` runs
**48.73°–58.15°** from LF, the whole row below the 59° rest end (from RF:
87.58°–92.09°, near-vertical). `counter_ramp` has no `width` key in 15 §2.3,
so 09 §4.13's **0.044** default applies — not the 0.060 "ramp + funnel" figure
an earlier draft here invented, which flattered α by nearly a full degree and
hid a difficulty-5 shot. 15 §2.4's own clearance record confirms the default:
it needs "a ramp mouth its **0.022** half-width + 0.0135".

Spread checks: 7 labeled shots (5–8 ✓); **LF owns 2** (`counter_ramp`,
`bur_targets`), **RF owns 4** (`left_orbit`, `shaker`, `order_window`,
`ger_targets`), the counter flipper owns the seventh. LF is one under §4.2's
"each flipper owns 3–5" floor — and that is the shipped table, not a slip
here: the two shots that would balance it are the two the launch windows put
out of LF's reach entirely (above). `bur_targets` backhands from LF at
θ 18.6° ≤ 30° ✓. **Exactly one difficulty-5 shot,** `counter_ramp` at
α 1.00°, which is the cap §4.2 sets; it gates no mode start — every mode on
this table starts at `order_window` (difficulty 3) — and Dinner Rush asks for
it once, as Phase 2's jackpot, which §4.2 allows ✓. Three difficulty-3s
(`left_orbit`, `shaker`, `order_window`) and three 1–2s (`ger_targets` 1,
`pie_targets` 1, `bur_targets` 2) ✓.

Four of the six layer-0 θ land in [15°, 75°] (24.1° down to `shaker`'s 16.9°).
The two the launch windows force onto RF do not: `order_window` at **14.4°**
and `ger_targets` at **8.7°**. Both are recorded as measured. The 15° floor
exists because rejects off a shallow shot come back down the middle, and that
is exactly the failure the greybox found below — `drains.center` 0.33, over
15 §2.7's 0.28 — and exactly what `facing_deg` 195 on the G-E-R bank fixed,
to a measured **0.26**. So these two rows ship with the `facing_deg`
mitigation and that drain number as their justification line in `design.md`
§Shots — the form §11 items 1 and 4 use for a surviving warning — and with the
standing §4.2 exception recorded alongside the LF/RF split. Fixing
either belongs in a §4.2 or 15 §2.3 change; re-labelling this map to make the
arithmetic come out is the same lie as measuring d down the lane. (The
counter's P-I-E fan
runs 2.3–17.3°, so its inner targets sit below the floor too and are exempt
outright — layer-1 mini-flipper shots with no drain beneath the reject path,
§4.2.)

**5–6. Validate + greybox autoplay.** `tb_validate` went clean after three
fixes: **V011** (the `counter_drop` chute did not meet the counter's front
wall — the wall was closed, so a layer-1 ball had no gravity exit; opened a
0.036 m gap at the chute head), **V006** (that gap was first authored at
0.030 m, inside the 6–34 mm trap band of §4.3; 0.036 m clears it), and
**V010** (the chute was first drawn `[0.235,0.700]` → `[0.235,0.762]`, a
0.062 m run for the same 0.048 m climb = grade 0.774, over the 0.60 ceiling;
dropping its foot to y 0.672 makes the run 0.090 and the grade 0.533). The
greybox reports (`--runs 20 --skill 1 --seed 1000 --balls 3` — the §1
iteration count, not the §0.7 acceptance run — plus the
`--seconds 300` coverage session) missed *this table's* declared bounds
(15 §2.7: 40–55 s, `drains.center` ≤ 0.28) even though both sat inside the
generic §8.3 rows:

```
ball_time_s.p50 = 34.2        FAIL (table band 40–55; §8.3 row 22–60 is green)
drains.center   = 0.33        FAIL (table band ≤ 0.28; §8.3 row < 0.35 is green)
```

Drain context clustered centre drains after `ger_targets` rejects — the bank
faced dead down-table. Fixes per the §8.4 centre-drain and short-ball-time
rows, one at a time: (1) rotated the `ger_targets` faces to `facing_deg` 195
so the bank no longer rejects along the drain line; (2) the ball-time row's
first three steps all touch 15 §0.4's shared standard bottom — sling
`kick_speed` 3.5, the outlane channel, the inlane rail — and Atomic Diner's
roster overrides none of them, so the first step that actually applied was
step 4, re-angle a face that rejects toward the drain: the `counter_hood`
deflector got its **14.0°** down-right lean (15 §2.3), turning `counter_drop`
exits into mid-field rolls instead of centre feeds. Second report:
`ball_time_s.p50 = 42.6` PASS, `drains.center = 0.26` PASS,
`coverage.share = 0.89` PASS — all layout rows green. Two lessons: a table's
own `meta.autoplay_bounds` band, not the §8.3 row, is what the author tunes
to; and when a §8.4 step would need a §0.4 value the table does not
override, you walk on down the row instead of inventing the override.

**7–8. Rules outline (15 §2.5).** Words: BURGER = B-U-R + G-E-R standups
(letters latch); FRIES = F-R-I top lanes + E-S inlane rollovers; SHAKE = 5
**`captive_full_travel{id}`** events with `id = "shaker"` — the canon event
(PLAN.md §5.7; payload owned by 10-scripting.md §4.1) the sim emits when the
captive ball reaches the far end `b` of its slot at ≥ 0.3 m/s. Every strike
also emits the ordinary `switch_hit{id, ball_id, speed, tags}` that pays the
40,000 shaker-hit row and feeds the 30-hit extra ball, but only full travel
spells a letter and no field of `switch_hit` — `speed` included — is read to
spell one. Orders are a 3-step ladder — Order 1 = BURGER, Order 2 =
BURGER + FRIES, Order 3 = all three words — each lighting `order_window`;
shooting it starts **Order Up!** (25 s, four lit delivery shots at 300,000,
all four = 1,500,000 × order number). The counter pays 150,000 per PIE
target, 750,000 for P-I-E complete plus one tip multiplier (max ×3, resets
at ball end). Milkshake Multiball: with SHAKE complete each further
`captive_full_travel{id}` on the shaker fills the blender 1/3; at 3/3
`order_window` starts 2 balls, jackpots shaker 400,000 / `counter_ramp`
600,000, super 2,000,000. Wizard
Dinner Rush = Orders 1–3 + Milkshake MB started, three phases, 8,000,000
payoff. Ball save 10 s; skill shot 500,000 (super 1,500,000); combos
400,000.

Economy check against §5.1 (target avg game 6,000,000): every base value
sits in its class band — switch/rollover 4,000 (switch), letters 25,000 and
shaker hits 40,000 (minor), PIE target 150,000 and delivery shots 300,000
(major/mode), order delivery 1,500,000 × order and Pie Case 750,000 (mode
award), jackpots 400,000–600,000 and the 2,000,000 super (inside §5.1's
super row of ≤ 3× the 1,000,000 jackpot cap, and the smallest of the five
shipped supers — no §11 item 4 justification line needed), Dinner Rush
8,000,000 (wizard). 15 % rule
at EGS 6.0e6 (cap 900,000): the worst individually repeatable labeled shot
is the PIE target at 150,000 × 4.0 makes = 600,000 = **10.0 %** ✓; the 18 %
"Order Up!" row is four *different* lit shots (≈ 0.9 makes each →
262,500 = 4.4 % per shot) ✓. Tips bonus (10,000 × letters × 3 balls) ≈ 3 %
of EGS — at the floor of the 3–15 % band ✓.

**9–12.** Art per §6 in three sub-passes: dark teal field, cherry-coral shot
arrows and boomerang lane trim, chrome counter; `glow_white` only in halo
cores; glow budget measured at 11 % of playfield area (§6 limit 15 %).
Audio per §7: `ad_bubble` pops, `ad_shake` shaker, `ad_bell` letters,
`ad_orderup` deliveries, `ad_doorchime` `ramp_made`, `ad_blender` jackpots,
`ad_plate` on `drain`; songs `attract` 96 BPM doo-wop, `main` 132 BPM swing,
`mode` + hand-clap backbeat, `multiball` 152 BPM rockabilly, `wizard` 160
BPM, `game_over` vibraphone tag — every 12-audio.md state covered, zero V037
errors and zero V038/V039 warnings. Final tuning loop green at skills 0/1/2
in both session shapes, first at `--runs 20` and then confirmed on 15 §0.7's
binding acceptance suite (three `--runs 500 --balls 3` sweeps at skills
0/1/2 plus the 300 s coverage session, one `--seed`, one uninterrupted run —
these are the numbers below): `score.p50` 6.0e6 (table band 4.5e6–7.5e6, §8.3 row
3.0e6–14.0e6), skill-0 p50 1.3e6 = 22 % of skill-2 (< 25 % ✓),
`modes.started_per_game` 2.4, `modes.multiball_reach_share` 0.38,
`modes.wizard_reach_share` 0.04 (table band 0.02–0.06 ✓), `stuck_balls` 0.
`meta.default_scores` carries the ten themed entries of 15 §2.5, from
30,000,000 CHF (= 5 × EGS) down to 6,000,000 JOE (= 1 × EGS). §11 checklist
recorded in `design.md` §Ship.

## 11. The ship checklist

Record pass/fail evidence for every item in `design.md` §Ship.

1. **Validator clean:** `tb_validate` exit 0; every surviving warning
   listed with a justification line.
2. **Autoplay green ×3:** all §8.3 metrics green, each read at the skill and
   in the session shape §8.3 assigns it. Iterate at `--runs 20 --balls 3` at
   skills 0, 1, 2 plus one `--skill 1 --seconds 300` session (§2 step 11);
   **ship on the binding acceptance suite of 15-launch-tables.md §0.7** —
   three `--runs 500 --balls 3` sweeps at skills 0, 1, 2 plus the same
   `--runs 1 --seconds 300` coverage session, all on one `--seed`, every
   target green on one uninterrupted suite. Record the 500-run numbers, not
   the 20-run ones.
3. **Screenshots reviewed:** all five views + one `--state` per mode
   reviewed against §8.5; zero unresolved findings.
4. **Economy within budget:** the `design.md` §Economy table names a §5.1
   class for every award and every base value sits in that class band. Any
   value that must sit outside its band is listed with a justification line,
   exactly as a surviving warning is in item 1 — no shipped table needs one:
   §5.1's super row (≤ 3× the jackpot cap) already covers all five supers,
   from atomic-diner's 2,000,000 through voltage-vandals' 2,500,000 to the
   3,000,000 of neon-drift, tilt-o-tron and cosmic-carnival. EGS in
   3.0e6–14.0e6 **and** inside the table's own declared `score.p50` band;
   15 % rule holds for every labeled shot; bonus 3–15 % of EGS.
5. **All events sounded:** zero **V037** errors (unresolved patch
   reference) and zero **V038** warnings (scoring event with no mapped
   sound); every custom mode event mapped in `audio.json`.
6. **Music states covered:** every 12-audio.md music state has a pattern
   assignment (explicit reuse allowed, silence not).
7. **Attract content:** attract light cycle defined, attract music pattern
   assigned, backglass attract art present (13-art-direction.md).
8. **Default high scores:** **exactly 10** entries in `table.json`
   `meta.default_scores`, themed three-glyph initials, scores positive
   integers **non-increasing** down the array (fewer or more, or an
   out-of-order score, is a hard V028 **error** — 09-table-format.md §8).
   Ladder shape: #1 ≈ 5 × the table's `score.p50` target, tapering to
   #10 ≈ 1 × `score.p50`, each rounded to a clean number (Atomic Diner:
   30,000,000 → 6,000,000 against a 6.0e6 target). Displayed per
   11-game-framework.md §7 (per-table top 10, two attract pages of 5).
9. **Rules card:** `meta.rules_card` — 3–6 lines: skill shot, how to start
   a mode, how to start multiball, what the wizard needs.

## Common pitfalls

- **Shipping the skeleton.** Instantiating a §4.1 recipe and reskinning it
  is the reskin failure this guide exists to prevent. Correct: the skeleton
  is the starting arrangement; the signature mechanic must add/replace
  elements and re-derive the shot map before step 5.
- **Furniture elements.** Placing a target/toy where it looks good instead
  of on a shot line. Correct: every scoring element lies on a labeled shot
  line with 15° ≤ θ ≤ 75° (§4.2 — only a layer-1 mini-flipper shot is exempt
  from the floor); if it isn't shootable, make it a decal in `art.json`.
- **Tuning by intuition instead of reruns.** Changing five parameters, then
  running autoplay once with a new seed. Correct: ≤ 3 changes per
  iteration, same `--seed`, compare reports field by field (§8.4).
- **Shipping on the iteration run count.** Declaring a table green because a
  `--runs 20` report was green — 20 runs is fast feedback, and a metric
  sitting on its band edge there will cross the edge on the next seed.
  Correct: `--runs 20` is for the loop; acceptance is
  15-launch-tables.md §0.7's suite — three `--runs 500 --balls 3` sweeps at
  skills 0, 1, 2 plus the 300 s coverage session, one `--seed`, all green on
  one uninterrupted run — and those are the numbers recorded in
  `design.md` §Ship and in the table's §x.7 (§1, §11 item 2).
- **Measuring a shot from a flattering point.** Taking d to wherever the
  lane happens to be convenient instead of to the mouth, which shrinks d and
  inflates α by a whole difficulty band. Correct: d runs sweet spot → mouth,
  and for an orbit the mouth is the lane centre at the instance's `entry_y`
  (§4.2; §10's `left_orbit` is 0.627 from `entry_y_left` 0.660, not 0.591
  from a point 40 mm lower).
- **Fixing score with ball time.** EGS is low, so the author lengthens
  ball save. Correct: EGS is fixed with the economy rows of the tuning
  matrix; ball-time knobs are for ball-time metrics only.
- **Scoring a decade off.** Writing a switch at 50 and a jackpot at
  200,000 — a self-consistent economy that no shipped table shares, so the
  player's score reads wrong on the same backglass. Correct: the §5.1 bands
  are absolute point values (switch 1,000–10,000 … wizard 5–15 M), and every
  base value in the `design.md` §Economy table must name its class.
- **Designing motion the engine has not got.** A toy that drags a target,
  a rotating turret, a physically moving bank. Correct: `toy` is a static
  collider plus art animation hooks (09-table-format.md §4.20) and there is
  no toy kinematics API in 10-scripting.md — motion in v1 means *which
  element is hot* moving (lights, magnets, gates), not geometry moving
  (§3 seed 3).
- **Bounds declared at the wrong skill or shape.** Mirroring `score.p50`
  into `meta.autoplay_bounds` and expecting the 300 s smoke to catch a
  regression — that session emits `null` for every game-scoped field, so the
  bound is skipped and the table looks green. Correct: §8.3 assigns each
  metric a skill and a session shape, every declared bound records its skill,
  and score/mode/shot bounds are gated by the `--balls 3` job (§8.2, §8.3).
- **Mandatory signature every ball.** Making the signature mechanic the
  only progress path makes skill-0 games miserable. Correct: switches,
  lanes, and pops always score (§5.1); the signature gates the *big* value
  only — verified by the skill-spread metric (§8.3).
- **SDTM element faces.** A drop bank, captive ball, or post face
  perpendicular to the drain rejects straight down the middle. Correct:
  angle every flat reject face ≥ 4° off dead-down-table (§8.4 catches it).
- **Light soup.** Every light blinking during every mode. Correct: one
  blinking light per active objective, one meaning per color (§6);
  everything else off or dim.
- **Layer-1 dead ends.** An upper playfield the ball can enter but only
  leave via a scripted kicker — a script error strands the ball. Correct:
  every layer-1 area has a gravity drain path to layer 0 (§4.3);
  V007/V010/V011 enforce reachability and ramp/layer continuity.
- **Ignoring skill 0.** Tuning only at skill 2. Correct: skill 0 is the new
  player; it must still hit every labeled shot occasionally (rate ≥ 0.02)
  and score < 25 % of skill 2 — both are gated metrics (§8.3).

## Done when

- [ ] `tb_validate` implements the full 09-table-format.md §8 V-code
      catalog (including the authoring-loop codes V032–V039) with 09 §10's
      line format and exit codes (`0` no errors / `2` errors / `3` IO).
- [ ] `tb_autoplay` implements the flip-window policy with the three skill
      profiles of §8.2 (reaction delay, timing σ, aiming, nudge exactly as
      tabled), seeded runs `S + run_index`, both session shapes, the
      `--check-bounds` / `--replay` / `--record-golden` behaviors and exit
      codes of §8.2, and emits every report field shown in §8.2.
- [ ] `tb_screenshot` renders the five views and `--state` variants of
      §8.5.
- [ ] All §8.3 target ranges are encoded in the autoplay report (each
      metric printed with its range, the skill and session shape §8.3
      assigns it, and a green/red verdict); game-scoped fields are `null`
      in a `--seconds 300` report, and `--check-bounds` evaluates a declared
      bound only when the run's skill and shape match it. A declared
      `shots[<id>].rate` floor below 0.10 at skill 2 is accepted only at
      0.08 and only for a shot whose sole feeder is a mini/upper flipper
      (§8.3); every other declared bound narrows its row.
- [ ] The two CI gate jobs exist and split as §8.3 says: the per-table
      300 s `--check-bounds` smoke (16-testing-ci.md §2.8) gates
      `ball_time_s.p50`, `drains.*`, `coverage.share`, `stuck_balls`; a
      separate `--balls 3` job gates `score.p50`, `modes.*`, and
      `shots[<id>].rate`, each at its assigned skill.
- [ ] A table failing a §8.3 metric, treated with the first applicable
      tuning-matrix row (§8.4), moves that metric toward range on the next
      same-seed run (spot-checked on test-lab with an injected defect).
- [ ] Atomic Diner (M16) was authored by following this guide's steps in
      order; its `design.md` records every step artifact, and the shipped
      pack matches §10 — which is itself a compressed retelling of
      15-launch-tables.md §2 (same roster ids, coordinates, words, orders
      and awards; on any divergence 15 wins and §10 is corrected in the same
      PR).
- [ ] Every shipped table passes the full ship checklist (§11) on one
      uninterrupted 15-launch-tables.md §0.7 acceptance suite (three
      `--runs 500 --balls 3` sweeps plus the 300 s coverage session, not a
      `--runs 20` iteration report), with evidence and all 12 step artifacts
      recorded in its `design.md` — including exactly 10
      `meta.default_scores` entries (§11 item 8, V028) and an economy whose
      base values sit in their §5.1 class bands, supers included (§5.1's
      super row covers all five shipped tables, so none carries a
      justification line — §11 item 4).
- [ ] The five shipped tables use five different signature mechanics
      (PLAN.md §5.8) and each passes the distinctiveness test in writing.
- [ ] A new table authored end-to-end using only this guide (PLAN.md §8
      Definition of Done) reaches ship-checklist green without reading tool
      source code.
