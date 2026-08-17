# 11 — Game Framework

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 05-engine-core.md (settings.json, input mapping, crash-safe file
rules, logging), 08-physics.md (nudge/tilt danger detection, trough and
plunger mechanics, hold states), 09-table-format.md (table.json `meta` fields,
element ids), 10-scripting.md (`tb.*` API, event payloads), 12-audio.md
(built-in SFX patches), 13-art-direction.md (backglass art layers, segmented
digit style), 15-launch-tables.md (per-table default high scores, replay
thresholds).

This document owns: the game state machine, players and multiplayer including
Duel mode, the ball lifecycle, tilt policy, the scoring ledger, high scores,
all menus, backglass content, and the framework/script responsibility split.
It is implemented in `/src/game` (target `tb_game`), milestones M10 and M18.

## 1. Scope and execution model

The game framework (`tb::game::GameFsm`) runs **on the sim thread** and is
stepped exactly once per 1000 Hz physics tick, in **phase 3 of the binding
4-phase tick** below. This is the same pipeline 10-scripting.md §2.2 states
for scripts, with the FSM step made explicit:

```
tick N:
  1. late-latch input; physics integration + sim event generation
     (08-physics.md §2.1)
  2. dispatch sim events to Lua handlers, in emission order; per event,
     handlers in registration order
  3. step the GameFsm: consume the InputCommands drained at the top of the
     tick and this tick's sim events
     (drain, ball_lock, danger_threshold, …), run transitions T1–T24.
     Every framework-originated script event — `game_start`, `ball_start`,
     `ball_end`, `player_up`, `game_end`, `tilt_warning`, `tilt`,
     `multiball_start`, `multiball_end`, `ball_save_expired`, `timer_tick`
     — is dispatched to Lua **synchronously, at the moment the FSM emits
     it, in emission order, within this same tick**; it is never queued for
     tick N+1
  4. fire expired script timers (deadline == N), ascending timer_id;
     lua_gc STEP; publish SimSnapshot + GameSnapshot
```

Synchronous phase-3 dispatch is what makes §4.5 step 2 implementable: the
`ball_end` handler runs inside the FSM's BonusCount entry, so a script's
`tb.add_bonus` call lands *before* the FSM reads the final bonus on the next
statement. It is likewise why the 2 → 0 multiball drain of §2.5 can emit
`multiball_end` and then `ball_end` on one tick, in that order.

The framework's **event emission order is part of the deterministic replay
record** (canon §5.3): a replay that reproduces a tick must reproduce the
same framework-event sequence in the same order, and the determinism suite
compares those sequences, not just final scores: 16-testing-ci.md §2.4.1
folds an event-sequence component — a rolling hash over the tick's emitted
sim-event and framework-event types and ids, in emission order — into
`state_hash`, so a reordered or dropped framework event diverges the hash on
the tick it happens, for every table.

All framework durations are therefore tick counts (1 s = 1000 ticks); the
framework never reads the wall clock. The single exception is the timestamp
written into high-score files (§7), which is stamped by the persistence code
on the main thread at write time and never read back into the sim.

Menu-grade input (navigation, Start, back) reaches the FSM as `InputCommand`
records: the main thread converts SDL events into commands and pushes them
onto an SPSC queue that the sim thread drains at the top of each tick (queue
type and key bindings per 05-engine-core.md). Flipper/plunger/nudge *play*
input additionally arrives through the raw-input late-latch path; the FSM
only gates whether the sim acts on it. The framework publishes
`GameSnapshot` (state, per-player scores, ball number, messages, timers)
alongside the physics `SimSnapshot`; render and backglass layers read only
snapshots, never game state directly.

## 2. The game state machine

### 2.1 States

```cpp
enum class GameState : uint8_t {
  Boot,           // one-shot app init
  Attract,        // no session; rotating attract pages
  TableSelect,    // carousel + mode select sub-phase
  Settings,       // service menu tree (reachable from Attract only, T4)
  GameStarting,   // session created, table script game_start
  BallReady,      // ball sitting in plunger lane
  BallInPlay,     // >= 1 ball in free play
  BonusCount,     // end-of-ball bonus collection
  PlayerChange,   // advance player/ball rotation
  HighScoreEntry, // initials entry for qualifying players
  GameOver,       // final scores / Duel match result
  Paused          // overlay state; remembers the state it froze
};
```

`Paused` and `Settings` are the only states that remember a return state.
`Boot` occurs exactly once per process.

```
Boot --> Attract <---------------------------------------------+
           |  ^ \                                              |
           v  |  --> Settings --> (back) Attract               |
        TableSelect                                            |
           |                                                   |
           v                                                   |
     GameStarting --> BallReady --> BallInPlay --+             |
           ^              ^            |  ^______| (multiball  |
           |              |            v          drains, >0   |
           |              |        BonusCount     balls left)  |
           |              |         /   |   \                  |
           |         PlayerChange <-    |    -> HighScoreEntry |
           |                            v            |         |
           +--(Start)-- GameOver <------+------------+---------+
                            |                       (timeout / all done)
                            +--(10 s timeout)--> Attract
```

### 2.2 Transition table

Every transition in the game is one of the rows below. Any event not listed
for the current state must be ignored (and logged at debug level).

| # | From | Trigger | To | Notes |
|---|------|---------|----|-------|
| T1 | Boot | all subsystems initialized, ≥1 valid table found | Attract | invalid tables are excluded with a warning log |
| T2 | Boot | zero valid tables / fatal init error | (error screen) | show error text on playfield; any key exits the app |
| T3 | Attract | Start press | TableSelect | |
| T4 | Attract | service chord (hold both flippers, press Start) or `F10` | Settings | return state = Attract; `F2` is taken by the physics debug overlay (05-engine-core.md §14.3) |
| T5 | TableSelect | Start press in mode sub-phase | GameStarting | table + mode chosen |
| T6 | TableSelect | plunger (back) in carousel sub-phase | Attract | |
| T7 | Settings | plunger (back) at tree root | previous state | settings persisted on exit (05-engine-core.md) |
| T8 | GameStarting | 2000-tick intro elapsed AND `game_start` dispatched AND trough serve commanded | BallReady | |
| T9 | BallReady | ball crosses plunger-lane exit switch (`ball_launched`) | BallInPlay | one-way; lane re-entry later does not revert |
| T10 | BallInPlay | drain leaving `balls_in_play(free) == 0` **AND** `in_plunger_lane == 0` **AND** no serve pending **AND** no ball save pending **AND** no lock release in progress | BonusCount | the ONLY route out of play; includes the tilted case. All five conditions are re-evaluated on every drain, at the close of every serve window, and as each staggered lock eject leaves its lock (§2.5) |
| T11 | BallInPlay | drain that leaves ≥ 1 free ball, or a ball in the plunger lane, or a serve pending | BallInPlay | accounting only (§4.4); this is also the multiball case |
| T12 | BallInPlay | drain with active ball save | BallInPlay | auto-serve + autolaunch (§4.3) |
| T13 | BonusCount | collection done; another ball/player remains | PlayerChange | |
| T14 | BonusCount | collection done; session over; ≥1 player qualifies for top 10 | HighScoreEntry | never in Duel |
| T15 | BonusCount | collection done; session over; nobody qualifies | GameOver | |
| T16 | PlayerChange | 2000-tick display elapsed; `player_up` dispatched; trough serve commanded | BallReady | |
| T17 | HighScoreEntry | all qualifying players committed (or 60 s timeout each) | GameOver | |
| T18 | GameOver | Start press | GameStarting | new standard 1-player game, same table |
| T19 | GameOver | 10 000-tick timeout or plunger press | Attract | |
| T20 | BallReady / BallInPlay | pause key | Paused | forbidden mid-round in Duel (§8.5) |
| T21 | BonusCount / PlayerChange | pause key, Duel only | Paused | "between rounds" allowance |
| T22 | Paused | Resume selected | previous state | 3-2-1 countdown, then sim unfreezes (§8.5) |
| T23 | Paused | Restart confirmed | GameStarting | same table, same players, same mode |
| T24 | Paused | Quit confirmed | Attract | session abandoned; NO high-score entry |

Tilt is **not** a state transition: it sets a per-ball `tilted` flag inside
BallInPlay (§5) and the ball ends via T10 as usual. Duel round timeout is
likewise a flag inside BallInPlay (§3.4).

### 2.3 Per-state entry/exit actions

| State | Entry actions | Exit actions |
|-------|---------------|--------------|
| Boot | init logging/config/displays/audio/input (05, 07, 12); scan `tables/`, validate packs; load per-table score files, and where one is missing or corrupt seed it from `meta.default_scores` — or leave the list empty if the table declares none (§7) | none |
| Attract | start attract page rotation (§8.2); play attract music of last-selected table (12-audio.md); release any loaded table sim **and its Lua state** — no script runs in Attract, the light show is framework/art-driven (§8.2) | stop attract music |
| TableSelect | build carousel from valid tables; select last-played table; render live miniatures (§8.3) | on T5: load selected table pack fully (geometry, script, art, audio) |
| Settings | snapshot current settings for cancel; build menu tree from settings schema (§8.4) | persist settings crash-safe; apply live-appliable settings |
| GameStarting | create session (§3.1); reset per-player state; create fresh per-player `tb.state` tables; run script top level (if not yet run), fire `game_start`; show "PLAYER 1 — BALL 1" | command trough eject |
| BallReady | fire `ball_start {player, ball_number}` (new balls only, not saves/adds); arm default ball save timer start deferred to launch (§4.3); Duel: pause round timer | none |
| BallInPlay | on first entry per ball: fire `ball_launched`, start ball-save countdown, Duel: start/resume round timer | none |
| BonusCount | fire `ball_end {player, ball_number, bonus, bonus_multiplier}`; freeze ledger; run bonus collection display (§4.5); cancel all script timers | reset per-ball state: bonus=0, multiplier=1, tilt warnings + danger state reset (08-physics.md), ball save disarmed, every `ball_lock` drained to the trough (§4.5 step 5) |
| PlayerChange | compute next (player, ball) (§3.1); if player differs: swap `tb.state`, fire `player_up {player, previous_player}`; display "PLAYER n — BALL m" or "SHOOT AGAIN" (extra ball); Duel: round intro card (§3.4) | command trough eject |
| HighScoreEntry | for each qualifying player ascending by player number: run entry UX (§7) | persist score files crash-safe |
| GameOver | fire `game_end`; show final scores (Duel: match result) | destroy session; unload nothing (table stays loaded for T18) |
| Paused | freeze sim ticks for physics + scripts (FSM keeps consuming commands); duck music to 30 % volume, stop looping SFX; show pause menu | restore audio; on Resume run 3-2-1 countdown before unfreezing |

Event payloads above are quoted **verbatim** from the catalog in
10-scripting.md §4, which owns every field name; this document never coins a
field. The framework-emitted ones used here are `ball_start {player,
ball_number}`, `ball_end {player, ball_number, bonus, bonus_multiplier}`,
`player_up {player, previous_player}` (`previous_player = 0` for player 1
before ball 1), `multiball_start {ball_count}`, `tilt_warning {count}`, and
the empty-payload `tilt` / `multiball_end` / `ball_save_expired`.

### 2.4 Input consumption by state

"—" = ignored. Raw flipper/nudge input always *reaches* the sim; the FSM and
tilt/Duel flags gate whether coils respond.

| State | Flippers | Start | Plunger | Nudge | Pause key |
|-------|----------|-------|---------|-------|-----------|
| Attract | prev/next attract page | → TableSelect (T3); chord → Settings (T4) | — | — | — |
| TableSelect | move carousel / toggle mode | select (T5 / enter mode phase) | back (T6 / to carousel) | — | — |
| Settings | move / adjust value | enter / commit | back / cancel edit | — | — |
| GameStarting | live (no ball) | add player (§3.1, P1B1 only) | — | — | — |
| BallReady | live (skill-shot use) | add player (P1B1 only) | charge/release plunger | active, accrues danger | → Paused (T20) |
| BallInPlay | live unless tilted/timeout | add player (P1B1 only) | plunger (lane re-entry) | active, accrues danger | → Paused (T20) |
| BonusCount | both held = skip animation | — | — | — | Duel only (T21) |
| PlayerChange | — | — | — | — | Duel only (T21) |
| HighScoreEntry | prev/next glyph | confirm glyph | backspace | — | — |
| GameOver | — | new game (T18) | → Attract (T19) | — | — |
| Paused | menu up/down | select | back = Resume | — | Resume |

### 2.5 Multiball and tilt paths through the machine

- **Bonus fires only after the LAST ball drains.** During multiball each
  drain takes T11 (no state change, no `ball_end`) until
  `balls_in_play(free)` reaches 0 with an empty plunger lane, no serve
  pending and no lock release still owing an eject, then T10 fires exactly
  once. If two balls drain on the same
  tick and the count goes 2 → 0, the framework fires `multiball_end` first,
  then proceeds to T10 in the same tick.
- **A drain during a serve window never ends the ball.** T10 additionally
  requires `in_plunger_lane == 0` and *no serve pending*. A **serve window**
  opens the tick the framework commands a trough eject (ball save relaunch,
  `tb.add_ball`, lock-replacement serve, §4.2/§4.4) and closes when that
  ball reaches the plunger-lane settle zone — at which point it is counted
  in `in_plunger_lane` instead, until `ball_launched`. The window is
  **bounded at 9000 ticks**: §4.2 re-kicks after each 3000-tick failure and
  teleports the ball to the settle position after the 3rd (3 × 3000), so
  "serve pending" can never latch forever. Autolaunch then fires 500 ticks
  after settle, so from serve command to `ball_launched` there is always
  ≥ 500 ticks (plus lane flight) during which a drain of the *other* balls
  is T11, not T10. Without this rule a drain in that window would end the
  ball in the middle of a multiball start. A ball merely *resting* in the
  lane (re-entry, or a serve the player has not plunged) blocks T10 too;
  that is not a hang — the player can always plunge, and after 30,000 ticks
  with no free ball the framework autolaunches it anyway (§4.2).
- **A lock release still owing balls never ends the ball.** T10's fifth
  condition: no forced release (tilt §5, Duel timeout §3.4, ball search
  §4.6) and no scripted `tb.release_lock` sequence may still owe an eject.
  Locks empty **one ball per 500 ms** (08-physics.md §6.14 owns the
  cadence), so with 3 balls locked at tilt the ejects land at +0, +500 and
  +1000 ms; ball A can be free and drained by +400 ms while B and C are
  still sitting in the lock. Checking only the other four conditions would
  fire T10 into BonusCount at +400 ms with two balls still locked —
  short-changing the next player and contradicting 08-physics.md §6.14
  ("no lock state can outlive the ball"). The framework therefore tracks
  `lock_release_owed` = balls a release sequence has committed to eject but
  has not yet ejected; it is > 0 from the tick the release is commanded
  until the last of those balls leaves its lock, and it gates T10 while
  > 0. Every eject re-evaluates T10, so the ball ends on the first drain
  at which all five conditions hold. This is **bounded, never a wait
  state**: a k-ball release finishes ejecting at (k − 1) × 500 ms, so with
  the default 4 physical balls the longest possible hold-off is 1500 ms —
  far inside the 30,000-tick budget of §4.2/§4.6 case B, which remains the
  backstop if a released ball then gets stuck.
- A tilted ball still traverses BallInPlay → BonusCount → PlayerChange;
  BonusCount displays "TILT" for 1500 ticks and collects 0 (§5).
- A ball save that triggers during BonusCount is impossible by construction:
  T10 requires "no ball save pending"; a drain with save active is T12 and
  never leaves BallInPlay.

## 3. Players and multiplayer (R6)

### 3.1 Session model

```cpp
struct PlayerState {
  uint64_t score        = 0;   // capped at 9'999'999'999 (§6)
  int      ball_number  = 1;   // 1..balls_per_game
  int      extra_balls  = 0;   // stacked, cap 3
  sol::table state;            // the script's tb.state for this player
  // per-ball, live only for the current player:
  uint64_t bonus        = 0;   // base bonus points accrued (tb.add_bonus)
  int      multiplier   = 1;   // bonus multiplier, 1..10 (tb.set_multiplier)
};
struct Session {
  GameMode mode;               // Standard | Duel
  std::vector<PlayerState> players;  // size 1..4 (Duel: exactly 2)
  int current_player = 0;      // index
  int balls_per_game;          // settings game.balls_per_game: 3|5, default 3
};
```

- A standard game starts with 1 player. **Each Start press while player 1 is
  on ball 1** (states GameStarting, BallReady, or BallInPlay, before player
  1's first BonusCount) **adds one player**, up to 4. Each add plays the
  built-in `add_player` SFX patch (12-audio.md) and updates the backglass
  immediately. Start presses after P1 ball 1 ends, or at 4 players, or in
  Duel, are ignored.
- Rotation: players alternate in order 1→2→…→N→1; `ball_number` increments
  when the rotation wraps back to player 1 — implemented as: after player
  p's ball ends without an extra ball pending, next player is `p % N + 1`,
  and that player's own `ball_number` is used (each player tracks their own,
  which also handles mid-game joins: players added during P1B1 start at
  ball 1 like everyone else).
- Session ends when every player has finished ball `balls_per_game` and has
  `extra_balls == 0`.

### 3.2 The `tb.state` swap (contract with 10-scripting.md)

The framework owns N Lua tables, one per player, created empty at
GameStarting. At every PlayerChange where the player differs, the framework
re-points the script-visible `tb.state` reference at the incoming player's
table **before** firing `player_up`. Scripts must never cache `tb.state` in
a local across ticks (10-scripting.md repeats this rule to authors). All
per-player rule progress (mode progress, lock credits, lane memory) lives in
`tb.state`; everything outside it is either per-ball (reset by the
framework, §4.5) or global to the table.

Physically locked balls are **not** per-player: balls held in `ball_lock`
elements stay where they are across player changes. Scripts track per-player
lock *credit* in `tb.state`; `tb.release_lock` releases only physically
present balls, and `tb.add_ball` covers any shortfall (mechanism in §4.4,
authoring pattern in 10-scripting.md).

### 3.3 Extra balls and replay

- Scripts award extra balls with `tb.award_extra_ball()` (signature in
  10-scripting.md). Effect: `extra_balls += 1` up to the stack cap of 3;
  awards past the cap post 100,000 points through the ledger instead.
  Award shows "EXTRA BALL" on the backglass and plays
  the built-in `extra_ball_fanfare` patch.
- At end of ball, if `extra_balls > 0`: decrement, **same player replays the
  same `ball_number`** ("SHOOT AGAIN" in PlayerChange). Bonus is still
  collected and per-ball state still resets.
- **Replay** (score threshold award): threshold per table from `table.json`
  `meta.replay_score` (values per 15-launch-tables.md; if absent,
  5,000,000), overridable per table in the service menu (persisted as
  `game.replay_score.<slug>` in settings.json). When a player's total score
  first crosses the threshold, award per setting `game.replay_award`:
  `"extra_ball"` (default — same path and cap as above) or `"off"`. One
  replay per player per game. Plays the built-in `knocker` patch.
- Extra balls and replay are framework mechanisms; scripts only decide
  *when* to call `tb.award_extra_ball()`.

### 3.4 Duel mode — full specification

Duel is a 2-player, best-of-3-rounds match on one table, selected in the
TableSelect mode sub-phase (§8.3).

**Structure.** The framework creates a standard-looking session: 2 players,
`balls_per_game = 3`. Round r = ball r; within a round, player 1 plays their
ball r, then player 2 plays theirs (normal rotation). Each ball is timed:
**75,000 ticks (75 s) of live play**.

**Timer rules.**
- The round timer counts down only while at least one ball is in free play.
  It **pauses whenever every ball is either in the plunger lane or held**
  (kicker hold, magnet hold, ball_lock — hold states per 08-physics.md), and
  during Paused. It never runs in BallReady.
- Timer visible on the backglass as `M:SS` plus a 10-segment depletion bar;
  final 10 s: digits flash at 2 Hz (subject to §10 flash reduction) and the
  built-in `timer_low` tick plays once per second.
- **Timeout:** when the timer reaches 0, the framework (a) freezes the
  ledger for this ball — subsequent `tb.score` and `tb.add_bonus` posts are
  discarded, (b) disables flippers framework-side (no `tilt` event, no
  warning), (c) **force-ejects every captured ball exactly as tilt does**
  (§5: kickers including script-held `tb.kick_hold` balls, magnets
  released, and every `ball_lock` emptied one ball per 500 ms), (d) waits
  for all balls to drain, then ends the ball normally. Because the lock
  emptying is staggered, T10's fifth condition holds the machine in
  BallInPlay until that release has ejected every ball it owes (§2.5) — an
  early drain cannot end the round while balls are still in a lock. Step
  (c) is
  mandatory for the same reason as in tilt: the timeout path waits for
  every ball to drain, and a held ball that nothing will ever release would
  hang the match. Bonus accrued before timeout still collects in
  BonusCount.
- **Drain before timeout forfeits the remaining time**: the ball simply ends
  (T10); there is no ball save in Duel.

**Scoring and rounds.**
- Round score for a player = ledger delta during that ball (base posts +
  collected `bonus × multiplier`). Cumulative totals are tracked but not
  used for the result.
- Higher round score wins the round (1 round win, shown as a pip). Exactly
  equal round scores void the round (no pip). The match ends the moment a
  player reaches 2 round wins; otherwise after round 3, more pips wins.
- **Tie-break:** if pips are equal after round 3 (1–1 with a void, 0–0,
  etc.), play sudden death: one **30,000-tick (30 s)** ball each, same
  rules, appended as ball 4 (then 5, … — repeat sudden death while still
  tied). Scripts see these as ordinary `ball_start` events with ball
  numbers beyond 3 and must treat ball numbers as opaque (10-scripting.md,
  14-authoring-guide.md).

**Scripts see a normal game.** Duel reuses `rules.lua` unchanged; there is
no API flag exposing Duel. Everything that differs is framework-side:

| Concern | Standard | Duel (framework-side) |
|---|---|---|
| Players | 1–4, Start adds | fixed 2; Start ignored in play |
| Balls per game | settings 3/5 | forced 3 (+ sudden-death balls) |
| Ball timer | none | 75 s (sudden death 30 s), pause rules above |
| Ball save | default 8 s + `tb.ball_save` | disabled; `tb.ball_save` is a no-op; default save not armed |
| Extra balls | stack ≤ 3 | `tb.award_extra_ball()` converts to +100,000 points |
| High scores | top-10 entry | skipped entirely (75 s scores are incomparable) |
| Pause | in BallReady/BallInPlay | only between rounds (T21) |
| Game over | final scores | match result screen |

**Backglass throughout Duel** (zones per §9): both players' *round* scores
large, round-win pips (●●○) under each, "ROUND n" and whose ball it is in
the status band, the countdown timer replacing the ball-number slot, ticker
for messages. PlayerChange shows a 3000-tick round card: "ROUND n — PLAYER
m" plus, for the second player of a round, "TARGET: <round score to beat>".
GameOver shows "PLAYER m WINS THE MATCH  w–l" for 10 s with both cumulative
totals below.

## 4. Ball lifecycle and the trough

### 4.1 Accounting invariant

The trough model and switches are physics elements (08-physics.md,
09-table-format.md); this section is the framework's bookkeeping over them.
With `total_balls` from `table.json` (default 4):

```
total_balls == in_trough + in_plunger_lane + balls_in_play(free)
             + held (kicker/magnet holds) + locked (ball_lock inventory)
```

The framework asserts this every tick in debug builds; a violation is a bug,
never "self-healing" code. A ball inside a **serve window** (§2.5) is always
counted in exactly one of `in_trough` (eject commanded, ball not yet spawned
on the plunger tip) or `in_plunger_lane` (spawned/settled, 08-physics.md
§6.15); "serve pending" is a framework flag gating T10, not a sixth bucket,
so it never perturbs this invariant.

### 4.2 Serve and launch

- **Serve:** on BallReady/PlayerChange exit the framework commands the
  trough to eject one ball into the plunger lane. If the ball has not
  reached the plunger-lane settle zone within 3000 ticks, re-kick; after 3
  failed kicks, teleport the ball to the settle position and log an error.
  The serve is "pending" from the command until the ball settles — at most
  3 × 3000 = 9000 ticks — and a pending serve blocks T10 (§2.5).
- **Launch:** the player charges and releases the plunger (08-physics.md).
  Crossing the lane-exit switch fires `ball_launched` and T9. A weak launch
  that rolls back into the lane does not revert state; the plunger remains
  usable.
- **Autolaunch:** the framework fires the plunger at full strength 500 ticks
  after the ball settles in the lane, without player input, in exactly these
  cases: ball-save return, `tb.add_ball` serves, lock-replacement serves
  (§4.4). Never for normal ball starts (Duel included — its timer is paused
  in the lane, so there is no rush).
- **Lane stall safety:** since a settled lane ball blocks T10 (§2.5), a
  BallInPlay state with zero free balls and a ball resting in the lane for
  30,000 ticks (30 s) autolaunches that ball at full strength and logs a
  warning — the same 30 s budget as §4.6 case B, so no wait state in
  BallInPlay can outlive 30 s without framework intervention. This never
  applies in BallReady, where the player owns the plunge.

### 4.3 Ball save

- Default policy: every ball starts with a **single-use 8000-tick (8 s)**
  ball save, counting from `ball_launched`. Duration configurable via
  settings `game.ball_save_seconds` (0–15 s, default 8; 0 disables).
- Per-table override: `tb.ball_save(ms, uses)` re-arms or disables the
  save at any time (0 disables; milliseconds = exact ticks at 1000 Hz; full
  signature in 10-scripting.md — e.g. a long multi-use save at multiball
  start). Script-armed saves count from the call.
- On a drain while a save is active with uses remaining: decrement uses,
  show "BALL SAVED" on the backglass, serve a ball with autolaunch (T12).
  No `ball_start` fires (it is the same logical ball); scripts that care see
  the `drain` event followed by no `ball_end`.
- When the timer lapses unused, fire `ball_save_expired`. Tilt cancels any
  active save (§5). Ball save never applies in Duel (§3.4).

### 4.4 Multiball accounting

- `tb.add_ball(n)` serves n balls from the trough with autolaunch (bounded
  by trough contents; shortfall is logged and ignored — validator rules in
  09-table-format.md keep authors honest).
- When `balls_in_play(free) + held` rises above 1, the framework fires
  `multiball_start {ball_count}` once (edge-triggered); when it falls back
  to 1, `multiball_end`. Each individual drain is reported by the **sim** as
  `drain {ball_id, balls_remaining}` (08-physics.md §6.15); the framework
  does not re-emit it.
- **Locks — capture is unconditional.** A FREE ball entering a `ball_lock`
  region while `held < capacity` is captured by the **sim**, with no
  framework gate and **no script confirm step** (08-physics.md §6.14 owns
  this; there is no confirm API and never was). The sim emits `switch_hit`
  then `ball_lock`; the framework moves the ball into lock inventory the
  moment it sees that event, in phase 3 of the same tick (§1).
  - A script that does **not** want the ball — an unlit lock, a lock that
    is already "full" in rules terms — calls `tb.release_lock(id, 1)` in
    its own `ball_lock` handler. This mandatory unlit-lock pattern is
    documented in 10-scripting.md; every table with a `ball_lock` must
    implement it.
  - **Sim failsafe (08-physics.md §6.14):** if a locked ball is neither
    released nor claimed within **3000 ms**, the sim auto-releases one ball
    and logs a warning — the lock analogue of the kicker `capture_ms`
    auto-eject, so a buggy or silent script can never softlock a ball here.
  - `tb.release_lock(id, n)` kicks out `min(n, physically locked)` balls,
    one per 500 ms (08-physics.md §6.14 owns the eject kinematics). While
    that sequence still owes an eject it blocks T10 (§2.5), so a release
    that starts as the last free ball drains cannot be cut in half by the
    end of the ball.
  - **At end of ball, whatever is still locked goes home.** Balls nobody
    released are moved straight to the trough during BonusCount (§4.5
    step 5) — bookkeeping only, no eject and no `drain` event — so `held`
    and `locked` are 0 on every `ball_start` and no lock state outlives the
    ball (08-physics.md §6.14).
  - **Replacement serve:** if a capture leaves zero free balls, no lane
    ball and no serve pending, and the game is not ending, the framework
    serves a replacement with autolaunch — standard pinball behavior,
    scripts must not have to handle it. The decision is taken **1000 ticks
    after the capture**, not on the capture tick: a declining script's
    `tb.release_lock` latches at tick N+1 and its first ball leaves within
    500 ms (08-physics.md §6.14), so the 1000-tick delay lets the decline
    resolve and cancels the replacement instead of putting an extra ball
    into play. It is well inside the 3000 ms sim failsafe above.
- **While a ball is in play, draining is the only way it returns to the
  trough.** The sole exception is outside play: the end-of-ball lock drain of
  §4.5 step 5, which runs in BonusCount and is a pure bookkeeping transfer
  (`locked` decrements, `in_trough` increments) with no eject, no outhole
  transit, and no `drain` event. In both cases **there is no trough-entry
  switch** — `trough` is a logical element with no events
  (09-table-format.md §4.18) and 08-physics.md §6.15 models no outhole →
  trough transit. The sim removes the ball at the outhole the instant its
  center enters the outhole capsule (slot freed, trough count incremented)
  and emits `drain {ball_id, balls_remaining}`; the framework decrements
  `balls_in_play(free)` on that `drain` event — exactly one decrement per
  event, never on a switch closure and never inferred from ball state.

### 4.5 End-of-ball sequence

Runs in BonusCount, in this exact order:

1. Freeze the ledger (no further base posts).
2. Fire `ball_end {player, ball_number, bonus, bonus_multiplier}` — payload
   verbatim from 10-scripting.md §4, carrying the bonus as of emission —
   dispatched **synchronously in phase 3 of this tick** (§1), so scripts may
   still call `tb.add_bonus` inside the handler (last chance); after it
   returns, bonus is final and that final figure is what step 3 counts.
3. Display collection: `BONUS <bonus> × <multiplier>` then count the product
   into the score in at most 25 visible steps over 2000 ticks, one
   `bonus_tick` sound per step; if bonus is 0, show "NO BONUS" for 800
   ticks. Holding both flippers collects the remainder instantly. If
   tilted: show "TILT" 1500 ticks, collect nothing.
4. Cancel all running script timers (`tb.timer` instances); scripts
   re-create timers in `ball_start`.
5. Reset per-ball state: `bonus = 0`, `multiplier = 1`, tilt warnings and
   08's danger state reset, ball save disarmed, **every `ball_lock` drained
   to the trough** (below). Drop-target banks are NOT auto-reset (table
   policy — scripts call `tb.drop_bank_reset`).
6. Transition per T13/T14/T15.

**Lock drain (part of step 5).** Any ball still physically held in a
`ball_lock` when the ball ends is returned to the trough by the framework as
a bookkeeping move: `locked` decrements, `in_trough` increments (§4.1), and
that is all — **no eject, no eject kinematics, no scoring, and no `drain`
event** for the player, so it costs no time and cannot extend BonusCount.
This is what makes 08-physics.md §6.14's "no lock state can outlive the
ball" literally true: `held == 0` and every §6.14 failsafe countdown is
clear on the next `ball_start`, and no table can carry lock inventory across
balls or players. It is the settled counterpart of T10's fifth condition —
T10 keeps the machine in BallInPlay while a *release sequence* is still
staggering balls out at one per 500 ms (§2.5), and this step disposes of the
balls nobody ever asked to release. Per-player lock *credit* lives in
`tb.state` (§3.2) and is untouched by either rule.

### 4.6 Stuck-ball recovery

**Case A — a free ball has stopped.** If **at least one ball is FREE** and
every free ball has speed < 0.01 m/s for 10,000 consecutive ticks while
BallInPlay and not in the plunger lane, a hold, or a valid rest zone
declared in `table.json` (09-table-format.md), the framework applies a
synthetic impulse of 0.15 m/s in a direction drawn from the sim RNG —
without any tilt-danger accrual. After 3 impulses without a switch closure,
it pulses every kicker and magnet once ("ball search"); after 2 failed
searches, it teleports the ball to the plunger lane and logs an error.
The "at least one ball is FREE" guard matters: with zero free balls the
speed condition is **vacuously true** (an empty set of balls trivially
satisfies it), and firing case A there would eject a deliberately
script-held ball seconds into a legitimate hold. Case A never runs when no
ball is free.

**Case B — no ball is free at all** (every ball held by a kicker/magnet,
locked, or in the trough). This is the state a never-released
`tb.kick_hold` produces, and no case-A impulse can reach it. Its own timer:
if **no ball is FREE and none is in the plunger lane for 30,000
consecutive ticks (30 s)** of BallInPlay, the framework runs a ball search
that **does eject kickers and locks** — every kicker (script-held ones
included, `hold_ticks` cleared) and every `ball_lock` (one ball per 500 ms)
at the element default `eject_speed`/`eject_angle_deg`, plus every magnet
released — exactly the force-eject of §5, and logs at **error** level with
the offending element ids. Its lock emptying is a release sequence like any
other, so T10 stays blocked until it has ejected every ball it owes (§2.5)
and the search cannot end the ball halfway through its own stagger.
The 30,000-tick counter is independent of case
A's 10,000-tick counter and resets the moment any ball becomes FREE or
enters the plunger lane; both are held frozen while Paused, in BonusCount,
and between balls (the phases where no ball is meant to be live).
A never-released script hold is therefore always recovered and always
*counted* — the error log line fails `tb_autoplay --check-bounds`
(14-authoring-guide.md), so it shows up as a table bug in CI — and is never
a hang.

All of this uses sim ticks and the sim PCG32, so replays stay deterministic.

## 5. Tilt

- 08-physics.md owns the nudge model and **tilt-danger detection** (the
  damped tilt bob with its warn threshold, plus the anti-abuse accumulator
  — 08-physics.md §7.2–§7.3). The sim emits a neutral `danger_threshold`
  sim event each time a danger threshold is crossed, re-arming per 08's
  spec. Its payload is exactly `{source, magnitude, crossing_index}` as
  defined by 08-physics.md §7.2: `source` (`BOB_WARN`/`BOB_HARD`/`ABUSE`)
  and `magnitude` are diagnostics the framework may use for presentation
  only, and `crossing_index` is the 1-based index of this crossing within
  the current per-ball danger state (reset with that state at BonusCount
  exit). Every `danger_threshold` counts the same regardless of `source`.
  The
  framework uses `crossing_index` only to detect dropped or duplicated
  events; the authoritative warning count is the framework's own (below).
  The sim itself never emits `tilt_warning` or `tilt` — those are
  script-facing events owned by the framework (canon §5.7). The framework
  never re-computes danger.
- The framework counts threshold events per ball. Settings
  `game.tilt_warnings` ∈ {1, 2, 3}, default **2**: the first
  `tilt_warnings` events each fire `tilt_warning {count}` to the script,
  flash "DANGER" on the backglass, and play the built-in `tilt_warning_buzz`
  patch.
  The next event is **TILT**.
- On tilt, the framework: fires `tilt` to the script; disables flippers and
  slingshot coils (balls still collide with them as static rubber); cancels
  any ball save; freezes the ledger (base posts and `tb.add_bonus`
  discarded); sets the `tilted` flag; shows "TILT" full-band on the
  backglass. Pop bumpers, kickers, and magnets are also de-energized (in
  the precise sense defined two bullets below). It
  also **suppresses cabinet-button `switch_hit` events** (ids
  `button_flipper_left`/`_right`/`button_launch`, tag `"button"`) for the
  rest of the ball, so scripts cannot score off buttons after a tilt; ball
  switch hits still reach scripts but post no points against the frozen
  ledger (08-physics.md §6 preamble, 10-scripting.md §4.1).
- **Every captured ball is force-ejected on tilt (binding).** On the same
  tick it sets the `tilted` flag, the framework commands *every* CAPTURED
  ball to eject at its element's default `eject_speed` /
  `eject_angle_deg`:
  - every `kicker` holding a ball — **including a ball taken over by
    `tb.kick_hold`**, whose script hold (`hold_ticks = 0`) is cleared;
    defaults `eject_speed` 3.0 m/s and the element's `eject_angle_deg`
    (08-physics.md §6.9);
  - every `ball_lock`, which empties completely — **locked balls release
    too**, one ball per 500 ms; defaults `eject_speed` 2.5 m/s,
    `eject_angle_deg` 270 (08-physics.md §6.14). Because that emptying is
    staggered, T10's fifth condition blocks end of ball until the release
    has ejected every ball it owes (§2.5): with 3 balls locked the ejects
    land at +0/+500/+1000 ms, so a first ball draining at +400 ms takes
    T11, not T10, and the ball ends only once B and C are out and drained
    too;
  - every energized magnet is released (10-scripting.md §3.4 already has
    the framework force-releasing magnets on `tilt`).

  Without this the game deadlocks. Script timers are frozen during tilt
  (10-scripting.md §3.6) and cabinet-button `switch_hit`s are suppressed
  (above), so a script-held ball has **no release path at all** — and
  `tb.kick_hold` has deliberately opted out of the sim's `capture_ms`
  failsafe (08-physics.md §6.9). Cosmic Carnival reaches exactly this on
  ball 1: its cannon holds the ball in `cannon_breech` and releases on a
  button press or a 5 s timer (15-launch-tables.md §4.3), both suppressed
  by tilt. The ball would never drain, T10 would never fire, and the game
  would hang.
- **"De-energized" means no NEW captures and no scripted kicks** — it does
  *not* mean "held balls stay held". For the rest of a tilted ball:
  kickers and ball_locks capture nothing new, pop bumpers and slingshots
  deliver no impulse, magnets do not energize, and script-issued
  `tb.kick` / `tb.kick_hold` / `tb.magnet_*` / `tb.release_lock` are
  no-ops. The sim's `capture_ms` auto-eject **keeps running during tilt and
  is never suppressed** (08-physics.md §6.9): a ball that the sim does
  capture anyway — on the tilt tick itself, or in a saucer it rolls into
  while draining — leaves on its own timer (default 800 ms) with no script
  action needed, which is precisely why tilt can never re-create the
  deadlock it just cleared.
  Force-ejected balls are accounting-neutral for the multiball edge
  detector: §4.4's `multiball_start` / `multiball_end` edges are suspended
  for the remainder of a tilted ball, so releasing three locked balls into
  a dying ball does not fire a phantom multiball.
  Nothing else changes state: the ball(s) drain under gravity, each drain
  follows T11/T10, and the multiball last-ball rule still applies.
- Tilt forfeits the bonus (§4.5 step 3) but never the score already on the
  ledger, and never ends the game by itself.
- Warnings and 08's danger state (bob + abuse accumulator) reset at every
  BonusCount exit (§4.5), i.e. tilt state is strictly per ball. In Duel,
  tilt additionally forfeits the remaining round time (the ball is ending
  anyway; no extra rule needed).

## 6. Scoring ledger

- Scripts post points with `tb.score(points)`, integer ≥ 0 (non-integers
  floored, negative posts warn and no-op — 10-scripting.md §3.1). The
  ledger applies posts as-is: `score += points`. The bonus multiplier never
  touches `tb.score` posts; playfield multipliers are a script concern —
  scripts scale the argument before posting (10-scripting.md §3.1,
  15-launch-tables.md §0.5).
- `tb.add_bonus(points)` accrues base bonus; collection at end of ball adds
  `bonus × multiplier` (§4.5), with `multiplier` set by
  `tb.set_multiplier(n)`, n clamped to 1..10 (10-scripting.md §3.1),
  per-ball, reset to 1 each ball. The multiplier applies to the end-of-ball
  bonus **only** — there is no separate bonus-X in v1.
- Scores are `uint64_t`, hard-capped at **9,999,999,999** (10 digits);
  posts that would exceed the cap clamp to it.
- **Display formatting:** thousands separators with commas, no leading
  zeros, except a zero score renders as `00` (classic look). Examples:
  `00`, `1,250`, `9,999,999,999`. The backglass uses the segmented-digit
  face from 13-art-direction.md; the same formatter (in `tb_game`, unit
  tested) is used everywhere a score is printed.
- Every ledger apply emits `ScoreEvent {player, delta_applied, new_total}`
  on the sim event ring (canon §5.4). The backglass consumes these to roll
  the displayed score toward the true total at up to 20 display steps per
  second; deltas ≥ 25,000 additionally trigger the score flash
  choreography (§9, capped by §10).
- Ledger freezes (discard posts silently, log at debug): during BonusCount
  after `ball_end` returns, after tilt, after Duel timeout.

## 7. High scores

- Per-table top 10, file `<prefpath>/scores/<slug>.json` (pref path per
  canon §5.9), written with the crash-safe temp-file + rename rule from
  05-engine-core.md, immediately after each commit:

```json
{
  "version": 1,
  "table": "neon-drift",
  "entries": [
    { "initials": "AAA", "score": 25000000, "date": "2026-08-16" }
  ]
}
```

- `initials` is exactly 3 glyphs from `A–Z 0–9 space`; `date` is the local
  date `YYYY-MM-DD` stamped at write time (not sim state, §1).
- **Qualification:** standard mode only (never Duel, never a quit session —
  T24). A score qualifies if the list has < 10 entries or the score beats
  the 10th; ties insert *below* existing equal scores. All qualifying
  players enter initials, ascending player number.
- **Entry UX (HighScoreEntry state):** three slots, cursor on slot 1, every
  slot pre-showing `A`. The glyph ring is `A…Z 0…9 space ‹` where `‹` is
  the backspace glyph. Right flipper = next glyph, left = previous
  (hold ≥ 500 ms: repeat at 10 glyphs/s). Start confirms the slot and
  advances; confirming slot 3 commits the entry. Plunger key = backspace
  (move back one slot); selecting `‹` and pressing Start does the same.
  After **60,000 ticks (60 s)** without input, the currently displayed three
  glyphs are committed as-is (so no input at all commits `AAA`).
  Position 1 of the list is labeled "GRAND CHAMPION" wherever it is shown.
- **Seeding, and tables that declare no defaults:** `table.json`
  `meta.default_scores` is **optional** — 09-table-format.md §2 owns the
  schema and validates it (exactly 10 `{initials, score}` entries when
  present, V028). On first run, or when the score file is missing or
  corrupt, the framework seeds the file from that table's
  `meta.default_scores` with `date` = seed date. **When
  `meta.default_scores` is absent the table's high-score list simply starts
  EMPTY.** There is no built-in ladder — not in this document, not anywhere
  in Tiltburst — and the framework never invents placeholder entries; the
  first ten scores anyone posts fill the list (with < 10 entries every score
  qualifies, per the rule above). The five launch tables all ship themed
  defaults (15-launch-tables.md); `test-lab` and a newly authored table
  need not, and an empty list is a valid, expected state everywhere a list
  is shown (§8.2 attract page, §8.3 carousel, §9.2 TableSelect). "Reset high
  scores" in the service menu (§8.4) restores exactly what seeding would
  have written: the declared defaults, or an empty list if there are none.

## 8. Menus

### 8.1 Navigation grammar (cabinet buttons only)

Every menu in the product uses exactly this grammar; no menu may require a
mouse or extra keys:

| Input | Meaning |
|---|---|
| Left flipper | previous item / decrement value / previous page |
| Right flipper | next item / increment value / next page |
| Start | select / enter / commit |
| Plunger key | back / cancel |

Held flippers repeat at 10 steps/s after 500 ms. `Escape` — the pause/back
key (`input.pause`, 05-engine-core.md) — is additionally a **universal back
alias** in every menu state: wherever this grammar or §2.4 routes the
plunger key to back, `Escape` does the same. When the plunger key is also
bound to a confirm action (the default desktop map binds plunger = `Space`,
which 01-product.md §6 lists as a confirm key in menus), the confirm
meaning wins and `Escape` is the back path; the plunger key acts as back
only where it has no such collision (e.g. a cabinet's dedicated plunger
button). Keyboard equivalents come from the input map (05-engine-core.md);
menus always work from SDL events (canon §5.4), so they function even if
raw input fails.

### 8.2 Attract content rotation

Pages on the backglass (single display: on the playfield, §9.3), looping:

| Page | Duration | Content |
|---|---|---|
| Logo | 8 s | Tiltburst logo art, neon/particle choreography (13-art-direction.md) |
| High scores | 8 s | top 10 of the last-selected table, paged: two pages of 5, 4 s each; GRAND CHAMPION highlighted |
| Rules card | 10 s | `meta.rules_card` text lines from the last-selected table (09-table-format.md) |
| Press start | 5 s | "PRESS START" pulsing at 1 Hz + "1–4 PLAYERS · DUEL MODE" |

Flippers page manually; manual paging resets the page timer. The playfield
display in Attract shows the last-selected table's art with its attract
light show.

**Attract is framework/art-driven. There is no Lua attract hook and no
attract-time `lua_State` in v1.** Entering Attract releases the loaded table
sim *and* its Lua state (§2.3), so by construction no script could run
here — any claim that rules files own the attract show is wrong. The
framework plays the show itself: it reads the table's light/insert layout
and art data and runs the choreography of 13-art-direction.md §7.2 (the 15 s
loop: GI/insert `breathe` wave, `chase` along the longest guide path, logo
`strobe` burst, dark beat), driving the same light and particle paths that
`tb.light_*` drives during a game, and clamped by §10 when
`reduce_flashing` is on. Per-table flavor lives in `art.json`, not in
`rules.lua`.

### 8.3 Table select and mode select

TableSelect has two sub-phases:

1. **Carousel** — one table centered: live-rendered art miniature (art
   layers only, no sim), table name, theme line, top high score with
   initials (the line reads "NO SCORES YET" when that table's list is empty
   — a table without `meta.default_scores` starts that way, §7). Flippers
   move (wrapping), Start → mode phase, plunger → Attract (T6).
2. **Mode** — two options over the same preview: `STANDARD GAME` /
   `DUEL (2P)`. Flippers toggle, Start starts the game (T5), plunger →
   carousel.

### 8.4 Settings / service menu

Reached via T4. The tree must expose **every key in settings.json** — the
key list is owned by 05-engine-core.md; this menu is generated from that
settings schema (key, type, range, default), so a new setting appears here
without menu code changes. Top-level sections:

```
SETTINGS
├── GAME       balls per game (3/5), tilt warnings (1–3),
│              ball save seconds (0–15), replay award (extra ball/off),
│              per-table replay score override
├── DISPLAY    assignments + rotation override (07-displays.md), brightness
├── INPUT      remap flippers/plunger/start/nudge/pause, nudge sensitivity
├── AUDIO      master/music/sfx volume, output device (12-audio.md)
├── ACCESSIBILITY  reduce flashing, ball outline (§10)
└── SERVICE    switch test (shows raw switch closures live),
               display test pattern, audio test tone,
               reset high scores (per table, with confirm),
               reset all settings to defaults (with confirm),
               build/version info
```

Value editing: Start enters edit mode, flippers change, Start commits,
plunger cancels. All changes persist crash-safe on menu exit (T7); display
and audio changes apply live where possible.

### 8.5 Pause

- **Key:** default `Escape` (remappable, key `input.pause` in
  05-engine-core.md). Cabinet builds bind a dedicated button; there is no
  chord fallback during play (Start must keep meaning "add player").
- Allowed in BallReady and BallInPlay (T20). In **Duel**, pause is
  disallowed during those states and instead allowed only between rounds —
  BonusCount and PlayerChange (T21); pressing pause mid-round in Duel does
  nothing.
- Semantics: physics and script ticks freeze (the FSM itself keeps ticking
  to run the menu); render keeps drawing the frozen `SimSnapshot` under the
  menu; music ducks to 30 %, looping SFX stop; all gameplay timers (ball
  save, Duel round, script timers) are tick-based and therefore freeze for
  free.
- Menu: `RESUME` / `RESTART GAME` (confirm → T23, same table, players,
  mode) / `QUIT TO MENU` (confirm → T24; the session is forfeit and no
  high-score entry occurs). Plunger = Resume.
- Resume runs a 3-2-1 countdown (1000 ticks per step, wall-paced by the
  main thread since sim is frozen) before unfreezing; flippers are live on
  the exact unfreeze tick.

## 9. Backglass content model

### 9.1 Zones

The backglass composition (rendered ~30 Hz per canon §5.4) is five fixed
zones over the table's backglass art layer (`art.json` `backglass` layer
set, 13-art-direction.md; fallback: table palette gradient + table name):

```
+------------------------------------------------------+
|                ART BACKGROUND (table pack)           |
|  +----------------------+  +----------------------+  |
|  | ►P1   12,345,670     |  |  P2      3,456,780   |  |   A: score zone
|  +----------------------+  +----------------------+  |      (1–4 segmented
|  +----------------------+  +----------------------+  |       scores, active
|  |  P3          1,200   |  |  P4         00       |  |       player marked ►
|  +----------------------+  +----------------------+  |       and enlarged)
|   BALL 2      2x        [MODE 0:42]     PLAYERS 4    |   B: status band
|  +------------------------------------------------+  |   C: mode/timer slot
|  |            J A C K P O T   R E A D Y           |  |   D: message ticker
|  +------------------------------------------------+  |
+------------------------------------------------------+
```

- **A — scores:** only as many score cards as players; the active player's
  card is ~1.6× and carries the ► marker. Digits are the segmented face
  (13-art-direction.md) rolled per §6.
- **B — status band:** ball number, multiplier when > 1, player count;
  Duel: round number, whose ball, round pips.
- **C — mode/timer slot:** script mode timers via `tb.backglass` (payloads
  in 10-scripting.md) and the Duel countdown. Empty when unused.
- **D — ticker:** one message at a time from `tb.show_message` /
  framework messages (BALL SAVED, TILT, EXTRA BALL, REPLAY, DANGER);
  framework messages preempt script messages; each shows ≥ 1200 ticks.

### 9.2 Content per state

| State | Backglass shows |
|---|---|
| Boot | Tiltburst logo, "LOADING", progress of table scan |
| Attract | §8.2 rotation |
| TableSelect | selected table's backglass art + its top-10 list |
| Settings | mirror of the menu tree (same content as playfield rendering) |
| GameStarting | zones A/B with fresh session, "PLAYER 1 — BALL 1" in D |
| BallReady | zones live; "SHOOT!" pulsing in D after 3 s idle in the lane |
| BallInPlay | zones live; script content in C/D |
| BonusCount | bonus count-up staged in C, tilt shows "TILT" full-band |
| PlayerChange | "PLAYER n — BALL m" / "SHOOT AGAIN" big, Duel round card (§3.4) |
| HighScoreEntry | glyph ring, three slots large, qualifying score + rank |
| GameOver | final scores all players; Duel match result; "GAME OVER" |
| Paused | dimmed zones + "PAUSED" (no gameplay info hidden) |

Attract choreography, palettes, and flash patterns are 13-art-direction.md's
domain; this document only fixes *what* is shown.

### 9.3 Single-display overlay variant

With one display (canon §5.9), the backglass content collapses into a
playfield overlay: a strip across the top 8 % of the portrait playfield
(the far end, above the table art) showing: active player score (full
formatting), `Pn`, `BALL m`, multiplier, the mode timer, and ticker messages
(D). During Attract, TableSelect, Settings, HighScoreEntry, and GameOver the
full backglass composition takes over the playfield display instead. The
overlay must never cover the play area below the top wall arch; 06-rendering
composites it after bloom so it stays legible.

## 10. Accessibility

- `accessibility.reduce_flashing` (bool, default `false`): when on, the
  framework clamps every strobe it controls — light blink periods
  (`tb.light_blink`) clamp to ≥ 333 ms (≤ 3 Hz), backglass/score flash
  choreography replaces hard on/off with ≤ 30 % intensity ramps, full-band
  flashes (TILT, jackpot) become 500 ms fades, and the Duel low-timer flash
  drops to 1 Hz. The clamp is applied at the framework/render boundary so
  scripts need no changes. 06-rendering.md applies the matching cap to
  particle strobe effects.
- `accessibility.ball_outline` (bool, default `false`): render every ball
  with a 2 px light outline plus 1 px dark ring, drawn above all art and
  bloom (implementation in 06-rendering.md; this doc owns the toggle and
  its exposure in §8.4).
- Both settings are plain settings.json keys (05-engine-core.md), live in
  the Accessibility menu section, and apply immediately.

## 11. Framework-vs-script responsibility table

"Policy" = decides when/whether/how much. "Mechanism" = executes it. This
table must stay in agreement with 10-scripting.md; a conflict is fixed by
PR per canon §5.10.

| Concern | Policy | Mechanism | Notes |
|---|---|---|---|
| Game states, ball rotation | framework | framework | scripts only observe events (§2) |
| Ball serve/autolaunch/trough | framework | framework (sim) | §4.2 |
| Ball save | framework default; script may override via `tb.ball_save` | framework | §4.3; no-op in Duel |
| Tilt warnings/tilt | framework (danger from 08-physics.md) | framework | scripts get `tilt_warning`/`tilt` for show only |
| Base scoring | script (`tb.score`) | framework ledger | §6 |
| Bonus multiplier | script (`tb.set_multiplier`) | framework | bonus only (§6); reset each ball |
| Bonus accrual + collection | script accrues (`tb.add_bonus`) | framework collects | §4.5 |
| Extra ball / replay | script awards EB; framework awards replay | framework | §3.3 |
| Modes, mode timers | script (`tb.timer`) | framework timer service | timers cancelled at ball end |
| Multiball start (`tb.add_ball`) | script | framework | events edge-fired by framework (§4.4) |
| Ball locks | none for capture — the sim captures unconditionally (08-physics.md §6.14); script owns lock *credit* in `tb.state` and declines an unwanted ball with `tb.release_lock(id, 1)` | sim capture + framework inventory, replacement serve, `tb.release_lock`; a release in flight blocks T10 (§2.5) and whatever is still locked drains to the trough at end of ball (§4.5 step 5) | §4.4; no confirm step, 3000 ms sim failsafe |
| Lights | script (`tb.light_*`) | framework/render | §10 clamps apply |
| Sounds (SFX) | script (`tb.play_sound`) + framework built-ins | audio (12-audio.md) | framework built-ins listed in 12-audio.md |
| Music | script (`tb.play_music`/`tb.stop_music`) | audio | attract music framework-selected (§8.2) |
| Attract light show | framework | framework, from `art.json` (13-art-direction.md §7.2) | no Lua attract hook, no attract-time `lua_State` (§8.2) |
| Backglass messages | script (`tb.show_message`, `tb.backglass`) | framework | framework messages preempt (§9.1 D) |
| Menus, settings, high scores | framework | framework | scripts have no access |
| Duel structure | framework | framework | scripts see a normal game (§3.4) |

## Common pitfalls

- **Firing bonus/`ball_end` on every multiball drain.** Wrong: during
  multiball, drains with balls remaining are T11 (accounting + `drain`
  event only). `ball_end` and BonusCount happen exactly once, after the
  LAST ball drains (§2.5).
- **Making tilt a state.** Tilt is a per-ball flag inside BallInPlay; the
  machine still exits through T10/BonusCount. Modeling it as a state
  duplicates every drain path and breaks multiball tilt.
- **Reading "de-energized on tilt" as "held balls stay held".** Tilt (and
  Duel timeout) force-ejects every captured ball, empties every lock, and
  lets `capture_ms` keep running (§5, §3.4). Leave a `tb.kick_hold` ball in
  its kicker and the ball never drains, T10 never fires, and the game hangs
  — script timers are frozen and button events suppressed, so nothing can
  release it.
- **Waiting for a script to "confirm" a lock.** The sim captures
  unconditionally (08-physics.md §6.14); there is no confirm API. A script
  that does not want the ball calls `tb.release_lock(id, 1)` in its
  `ball_lock` handler, and the sim auto-releases after 3000 ms if nobody
  does anything (§4.4).
- **Ending the ball on a drain during a serve window.** T10 needs
  `balls_in_play(free) == 0` *and* an empty plunger lane *and* no serve
  pending (§2.5). Skip the last two and a drain lands mid-`tb.add_ball` or
  mid-lock-replacement and kills the ball as multiball is starting.
- **Ending the ball while a lock is still emptying.** Forced releases are
  *staggered* at one ball per 500 ms, so T10's other four conditions all go
  true long before the lock is empty: tilt with 3 balls locked ejects at
  +0/+500/+1000 ms, and the first ball can drain around +400 ms with two
  balls still sitting in the lock. Checking only free balls, the lane, the
  serve and the save fires BonusCount there, strands B and C for the next
  player, and makes the whole behavior timing-dependent. T10 has a **fifth**
  condition — no release sequence still owes an eject (§2.5) — and it is
  re-evaluated as each staggered ball leaves.
- **Letting lock inventory survive the ball.** Balls nobody released are
  moved to the trough during BonusCount (§4.5 step 5, bookkeeping only).
  Leave them locked and the next player starts a ball short, `held` is
  non-zero at `ball_start`, and 08-physics.md §6.14's "no lock state can
  outlive the ball" is false.
- **Deferring framework events to the next tick.** `ball_start`,
  `ball_end`, `tilt`, `multiball_*` and friends dispatch synchronously in
  phase 3, in emission order (§1). Queue them and §4.5 step 2 breaks:
  `tb.add_bonus` from the `ball_end` handler arrives after the bonus was
  already counted.
- **Inventing a trough-entry switch.** The trough has no events
  (09-table-format.md §4.18); the ball is removed at the outhole and the
  framework decrements on the `drain` event alone (§4.4). Waiting for a
  transit that never happens leaves `balls_in_play` stuck above 0 forever.
  The one non-drain path into the trough — the end-of-ball lock drain of
  §4.5 step 5 — closes no switch and emits no `drain` either; it moves the
  counters directly.
- **Running stuck-ball recovery when no ball is free.** "Every free ball is
  slow" is vacuously true with zero free balls, so the unguarded test
  ejects a legitimately script-held ball 10 s into a hold. Case A needs a
  free ball; the all-held case is case B's own 30,000-tick timer (§4.6).
- **Consuming a drain-with-ball-save as end of ball.** T12 stays in
  BallInPlay, serves with autolaunch, and fires no `ball_start`.
- **Letting scripts cache `tb.state`.** The framework swaps the reference
  at PlayerChange; a cached local silently corrupts another player's game.
  Enforce via the 10-scripting.md contract and a multiplayer swap test.
- **Wiring gameplay timers to wall time.** Every duration here is sim
  ticks; wall-clock timers break determinism, replays, and pause. Only
  high-score dates and the resume countdown use the wall clock (§1, §8.5).
- **Incrementing a shared ball counter on rotation.** `ball_number` is
  per-player: players join during P1B1 and each still gets 3/5 balls.
- **Giving Duel its own script API or state machine.** Duel differs only in
  the framework-side rows of §3.4's table; scripts must run unchanged and
  cannot detect Duel.
- **Extra ball advancing `ball_number`.** SHOOT AGAIN replays the same
  ball number for the same player; bonus still collects and per-ball state
  still resets.
- **Expecting the ledger to multiply `tb.score` posts.** The bonus
  multiplier applies exactly once, at bonus collection (§6); `tb.score`
  posts land unmultiplied. Playfield multipliers are a script concern —
  scale the points before posting — but never pre-multiply `tb.add_bonus`
  amounts. Double-multiplying is invisible in casual testing and wrecks
  table balance.
- **Blocking the sim thread in menus.** Menu states tick like any other;
  input arrives as queued commands (§1). Never run a nested event loop.
- **Promising script attract hooks.** Attract has no `lua_State` at all
  (§2.3, §8.2); the show is framework/art-driven from `art.json`
  (13-art-direction.md §7.2). Any design that calls into `rules.lua` during
  Attract cannot work.
- **Writing score files in place.** Use the 05-engine-core.md temp+rename
  rule; a crash during write must never lose the existing top 10.
- **Inventing a default high-score ladder.** `meta.default_scores` is
  optional; a table that omits it starts with an **empty** list (§7). There
  is no built-in ladder to fall back on, here or anywhere else in the
  product. Seed placeholders and the author's first real score looks like it
  ranked eleventh.
- **Renaming event payload fields.** Field names are quoted verbatim from
  10-scripting.md §4 and nowhere else: `ball_end {player, ball_number,
  bonus, bonus_multiplier}`, `player_up {player, previous_player}`,
  `ball_start {player, ball_number}`, `multiball_start {ball_count}`.
  Shorten `ball_number` to `ball` in the framework and `ev.ball_number` is
  `nil` in every rules file that follows the API doc.
- **Start during play doing nothing on ball 1.** Start must add players
  through the whole of player 1 ball 1 (GameStarting, BallReady,
  BallInPlay) — not just in a "lobby".

## Done when

- [ ] `GameFsm` implements exactly the states of §2.1 and transitions
      T1–T24; a table-driven unit test walks every row, and unlisted
      (state, event) pairs are ignored and logged.
- [ ] A 4-player standard game works end to end on `neon-drift`: Start
      pressed 3 times during P1B1 yields 4 players; rotation, per-player
      scores, ball numbers, and `tb.state` swap verified by an automated
      script-driven test.
- [ ] Tick order: framework events dispatch synchronously in phase 3 of the
      same tick, in emission order (§1) — a test script that calls
      `tb.add_bonus` inside its `ball_end` handler sees that bonus included
      in the collected total (§4.5 step 2), and the recorded framework-event
      sequence is byte-identical across two replays of the same input.
- [ ] Event payloads match 10-scripting.md §4 field-for-field: a Lua test
      handler asserts `ball_end{player, ball_number, bonus,
      bonus_multiplier}`, `player_up{player, previous_player}` (0 for
      player 1 before ball 1), `ball_start{player, ball_number}` and
      `multiball_start{ball_count}`; a missing or extra field fails.
- [ ] Multiball test: with 3 balls in play, two drains produce T11 +
      `drain` only; the final drain produces `multiball_end`, `ball_end`,
      one BonusCount; simultaneous 2→0 drain handled per §2.5.
- [ ] Serve windows: a drain while a `tb.add_ball` serve is pending, and a
      drain while the served ball is still sitting in the plunger lane,
      both take T11 and do **not** end the ball; the ball ends only once
      all five T10 conditions hold — no free balls, empty lane, no pending
      serve, no ball save, no lock release still owing an eject (§2.5).
      Includes the lock-replacement serve of §4.4.
- [ ] Lock release blocks end of ball: with 3 balls in one `ball_lock` and
      no free ball, a `tb.release_lock(id, 3)` (and, separately, a tilt)
      ejects at +0/+500/+1000 ms; the first ball draining at ~+400 ms takes
      T11, T10 fires only after the third ball is out and drained, and
      BonusCount is entered exactly once. Asserted on eject/drain ticks, not
      on wall time.
- [ ] Ball locks: capture happens with no script involvement; a script that
      calls `tb.release_lock(id, 1)` in its `ball_lock` handler gets the
      ball back and **no** replacement ball is served; a script that
      ignores `ball_lock` entirely sees the 3000 ms sim auto-release with a
      warning; a capture that empties the playfield serves a replacement
      with autolaunch 1000 ticks later.
- [ ] End-of-ball lock drain: a ball that ends with 2 balls still locked
      leaves `locked == 0` and `held == 0` at the next `ball_start`, with
      those balls counted in the trough (§4.1 invariant holds every tick),
      no `drain` event and no score posted for them (§4.5 step 5); the next
      player gets a full complement of balls.
- [ ] Ball save: default 8 s save returns a drained ball with autolaunch
      and no `ball_start`; `tb.ball_save(0)` disables; `ball_save_expired`
      fires when unused.
- [ ] Tilt: with `game.tilt_warnings = 2`, the third danger-threshold event
      tilts; flippers/slings/pops/kickers/magnets de-energize in the §5
      sense (no new captures, no scripted kicks); ledger frozen; bonus
      collects 0; next ball starts with warnings and 08's danger state
      reset. Settings values 1 and 3 also tested.
- [ ] Tilt force-eject (no deadlock, and no early end): tilting with a
      `tb.kick_hold` ball in a kicker and 2 balls in a `ball_lock` ejects
      all three at the element defaults (kicker immediately, lock at +0 and
      +500 ms), **T10 does not fire while the lock still owes an eject**
      even if the first ball drains first, then every ball drains, T10 fires
      exactly once and the ball ends — verified on Cosmic Carnival with the
      ball held in `cannon_breech` on ball 1 (§5). A ball captured on the
      tilt tick still auto-ejects on `capture_ms`. No `multiball_start`
      fires for the released balls. The test asserts the T10 tick, so it
      cannot pass by luck of drain timing.
- [ ] Stuck-ball watchdog, zero-free-ball case: a script that holds a ball
      with `tb.kick_hold` and never kicks triggers case B's 30,000-tick
      timer, which ejects kickers and locks, logs at error level, and lets
      the ball drain; case A's impulse never fires while no ball is free
      (§4.6).
- [ ] Ledger: `tb.score` posts land unmultiplied; bonus collects at
      `bonus × multiplier` exactly once, with the multiplier clamped to
      1..10 and reset each ball; score caps at 9,999,999,999; formatter
      renders `00`, `1,250`, and the cap with commas; `ScoreEvent`s reach
      the backglass ring.
- [ ] High scores: qualification, tie insertion below equals, entry UX
      (flipper ring incl. `‹`, Start confirm, plunger backspace, hold
      repeat), 60 s timeout committing displayed glyphs, crash-safe
      persistence (kill -9 during write leaves a valid file), seeding from
      `meta.default_scores` when the table declares it, and a table that
      declares none (e.g. `test-lab`) starting with an **empty** list —
      no built-in ladder — that displays correctly and accepts the first
      posted score at rank 1 (§7).
- [ ] Duel: full match playable — 75 s timer pausing in the plunger lane
      and during kicker holds, timeout freezing the ledger, killing
      flippers and force-ejecting every captured/locked ball (§3.4 step c)
      so the ball always ends — and ends only after the staggered lock
      release has finished, per T10's fifth condition — early drain
      forfeiting time, round pips,
      2-win match end, void rounds, 30 s sudden death on a tie, no ball
      save, no high-score entry, pause only between rounds — all with an
      unmodified `rules.lua`.
- [ ] Menus: Attract rotation with manual paging (running with no table
      `lua_State` loaded — the light show is framework/art-driven, §8.2);
      TableSelect carousel + mode phase; Settings tree generated from the
      settings schema covering every settings.json key; everything operable
      with flippers/Start/plunger only.
- [ ] Pause: freezes sim and all tick-based timers, ducks music, offers
      resume/restart/quit per §8.5, runs the 3-2-1 resume countdown, and
      is correctly restricted in Duel.
- [ ] Backglass shows the §9.2 content in every state on a two-display
      setup, and the single-display overlay variant shows the §9.3 strip
      without covering the play area.
- [ ] Accessibility: `reduce_flashing` clamps blink/flash behavior per §10
      (verified with a scripted 10 Hz `tb.light_blink` clamped to 3 Hz);
      `ball_outline` renders the outline above art and bloom.
- [ ] Determinism: a recorded input stream replayed over a full 2-player
      game (including a tilt and a multiball) reproduces identical final
      scores and identical event sequences.
