# 03 — Development Process & Autonomous Operation

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 02-decisions.md (ADR log), 04-milestones.md (what each PR
contains), 16-testing-ci.md (CI workflow content, test conventions, perf
gates).

This document defines how every line of Tiltburst is written, reviewed, and
merged. The implementor LLM must follow it literally. Where this document
gives a command, run that command; where it gives a template, fill that
template. "The reviewer" always means the automated **GLM 5.3 Code Review**
workflow at `.github/workflows/zai-code-review.yml` (already installed;
triggers on PR `opened`, `reopened`, `synchronize`, `ready_for_review`; skips
draft PRs and fork branches; requires the `ZAI_API_KEY` repository secret).

## 1. Repository conventions

### 1.1 Branches, commit messages, PR titles

- One milestone = one branch = one PR (splitting rules in §6).
- Branch name: `milestone/M<NN>-<slug>` where `<NN>` is the milestone number
  zero-padded to two digits and `<slug>` is 2–4 lowercase hyphenated words
  from the milestone title. Examples: `milestone/M00-scaffold`,
  `milestone/M05-table-format`, `milestone/M13a-art-engine`.
- PR title: `M<NN>: <exact milestone title from PLAN.md §6>` (for splits,
  `M13a: Art system, particles & Neon Drift beauty pass (part a: engine)`).
- Commit messages are Conventional Commits:

```
<type>(<scope>): <imperative summary, ≤ 72 chars>

<optional body: what and why, wrapped at 72>

Refs: M<NN>
```

| Field | Allowed values |
|---|---|
| `type` | `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `build`, `ci`, `chore`, `style` |
| `scope` | a module dir (`core`, `platform`, `sim`, `render`, `audio`, `table`, `game`, `tools`), a table slug (`neon-drift`), or `plan` for docs/plan edits |

Every commit must compile and pass `tb_tests` locally. Never force-push a
branch after the first review run has started — always append commits; history
is squashed at merge anyway.

### 1.2 CHANGELOG.md

`CHANGELOG.md` at the repo root follows Keep a Changelog 1.1.0 and is updated
in **every** PR. Seed content (committed at M0):

```markdown
# Changelog

All notable changes to Tiltburst are documented here. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: the
project versions the product, not the library API; v1.0.0 is milestone M20.

## [Unreleased]
```

Each PR adds bullet points under `## [Unreleased]` in the appropriate
subsection (`### Added`, `### Changed`, `### Fixed`, `### Removed`), prefixed
with the milestone id, e.g. `- M05: table.json loader with prefab expansion`.
M20 renames `[Unreleased]` to `[1.0.0] - <date>` and opens a fresh
`[Unreleased]` header.

### 1.3 Naming conventions

| Entity | Convention | Example |
|---|---|---|
| Types (class/struct/enum/using) | PascalCase | `FlipperParams`, `SimSnapshot` |
| Functions and methods | snake_case | `sweep_circle_vs_segment()` |
| Local variables and parameters | snake_case | `contact_normal` |
| Private/protected data members | `m_` + snake_case | `m_tick_count` |
| Public members of aggregate structs | snake_case, no prefix | `ball.pos` |
| Constants (`constexpr`, `const` globals/statics) | `k` + PascalCase | `kMaxBallSpeed` |
| Enumerators (`enum class`) | PascalCase | `GameState::Attract` |
| Macros | `TB_` + UPPER_SNAKE | `TB_ASSERT` |
| Files | snake_case `.h` / `.cpp`; tests `<topic>_test.cpp` | `table_loader.cpp` |
| Namespace | everything in `tb`; nested per-module namespaces (`tb::sim`) are allowed, not required | `tb::sim::Solver` |

Headers use `#pragma once`. Header include paths are quoted and relative to
`/src`: `#include "sim/solver.h"`.

### 1.4 .clang-format

Commit exactly this file as `/.clang-format` at M0. CI pins **clang-format
18** for the format check (workflow content in 16-testing-ci.md); do not use
options newer than 18.

```yaml
---
Language: Cpp
BasedOnStyle: LLVM
Standard: c++20
ColumnLimit: 100
IndentWidth: 4
TabWidth: 4
UseTab: Never
AccessModifierOffset: -4
NamespaceIndentation: None
FixNamespaceComments: true
CompactNamespaces: false
PointerAlignment: Left
ReferenceAlignment: Left
DerivePointerAlignment: false
BreakBeforeBraces: Attach
AllowShortBlocksOnASingleLine: Empty
AllowShortCaseLabelsOnASingleLine: false
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
AllowShortLambdasOnASingleLine: All
AllowShortLoopsOnASingleLine: false
AlwaysBreakTemplateDeclarations: Yes
BinPackArguments: false
BinPackParameters: false
PackConstructorInitializers: NextLine
Cpp11BracedListStyle: true
EmptyLineBeforeAccessModifier: LogicalBlock
IndentCaseLabels: false
IndentPPDirectives: None
InsertNewlineAtEOF: true
KeepEmptyLinesAtTheStartOfBlocks: false
MaxEmptyLinesToKeep: 1
SeparateDefinitionBlocks: Always
SortIncludes: CaseSensitive
IncludeBlocks: Regroup
IncludeIsMainRegex: '(_test)?$'
IncludeCategories:
  - Regex: '^"'
    Priority: 1
  - Regex: '^<(SDL3|sol|lua|miniaudio|nlohmann|fmt|stb|gtest|gmock)'
    Priority: 2
  - Regex: '^<'
    Priority: 3
...
```

### 1.5 Include order

Enforced by the `.clang-format` regroup rules above; write includes in this
order, one blank line between groups:

1. The header this `.cpp` implements (`foo.cpp` → `#include "sim/foo.h"`),
   matched automatically (test files `foo_test.cpp` also match `foo.h`).
2. Other project headers, quoted (`#include "core/log.h"`).
3. Third-party headers, angled (`#include <SDL3/SDL.h>`,
   `#include <fmt/format.h>`).
4. Standard library headers, angled (`#include <vector>`).

Never include third-party or platform headers from `/src/sim` or `/src/core`
public headers except `fmt` in `core` — `tb_sim` depends only on `tb_core`
(PLAN.md §5.1) and must stay headless.

### 1.6 Error-handling policy

- **Exceptions are allowed only during startup and asset/table load**: config
  parse, table pack load (`table.json`, `art.json`, `audio.json`,
  `rules.lua` compile), shader/font load, device creation. Each such phase is
  wrapped at its call site; a caught exception is logged and converted to a
  clean failure (skip the table, fall back, or exit with a message). No
  exception may propagate out of `main`.
- **The sim hot path is exception-free and allocation-free.** After table
  build completes, `Solver::step()` and everything it calls (collision,
  elements, script event dispatch glue) must not: throw, allocate
  (`new`/`malloc`/container growth), take locks other than the documented
  snapshot/ring-buffer primitives, perform I/O, or log per-tick (events go
  through the pre-sized `SimEvent` ring). All pools and arrays are sized at
  table build. The same rules apply to the audio device callback.
  16-testing-ci.md specifies the allocation-hook test that enforces this.
- **Lua errors never crash the game**: handlers run as protected calls; a
  failing handler is logged (identical (handler, message) pairs at most once
  per 5 s) and disabled after **10 consecutive failing invocations**, while
  the remaining handlers for that event still run and play continues. The
  one immediate-disable case is an instruction-budget overrun: it disables
  the offending handler permanently for the rest of the game and skips the
  tick's remaining handler invocations (10-scripting.md §2.4–2.5 is
  normative).
- **Asserts** use `TB_ASSERT`, defined in `src/core/assert.h`:

```cpp
// src/core/assert.h
#pragma once

namespace tb::detail {
[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg);
} // namespace tb::detail

#if defined(TB_DEBUG)
// Logs expression, file:line, and optional message via tb::log, then aborts.
#define TB_ASSERT(expr, ...) \
    ((expr) ? (void)0 : tb::detail::assert_fail(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
#else
// Compiled out of release hot paths entirely; expr is NOT evaluated.
#define TB_ASSERT(expr, ...) ((void)0)
#endif

// Always-on check for startup / load / tool code (never in the sim hot path
// or audio callback). Active in all build types.
#define TB_CHECK(expr, ...) \
    ((expr) ? (void)0 : tb::detail::assert_fail(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
```

`TB_DEBUG` is defined by CMake in Debug configuration only. Because
`TB_ASSERT` does not evaluate its expression in release, never put side
effects inside it.

### 1.7 TODO discipline

Every `TODO` in committed code must carry a target: `// TODO(M12): render on
backglass once displays land`. A TODO whose target milestone has merged is an
orphan and blocks merge (per-PR checklist, §4). Free-floating `TODO`, `FIXME`,
`XXX`, `HACK` without a milestone tag are forbidden.

### 1.8 Branch protection on `main`

`main` is protected so the six required CI checks of 16-testing-ci.md §3 are
mandatory for every merge. This document owns the **setup**;
16-testing-ci.md owns **which** checks exist.

**When it runs:** at **M0**, from the M0 PR's branch, immediately after that
PR's first fully green CI run and before `gh pr merge` — the check names only
exist once `build-test.yml` has actually run once. Re-run the same `PUT`
(it is idempotent, and replaces the whole protection object) whenever a
required job is added or renamed, and note the change in `docs/JOURNAL.md`.

**Step 1 — read the context names verbatim.** Never retype a check name from
prose (this document, 16-testing-ci.md, or the workflow YAML): the context
string is the job name *as GitHub reports it*, including the matrix suffix
with its exact spacing and parentheses.

```bash
pr=$(gh pr view --json number --jq .number)
gh pr checks "$pr"        # copy the name column verbatim into the JSON below
```

The M0 set is six contexts — confirm every one of them against that output:
`format`, `build-test (ubuntu-latest)`, `build-test (windows-latest)`,
`build-test (macos-latest)`, `asan`, `perf-gates`. The GLM 5.3 review
workflow is **never** a required context: it self-skips on drafts and on a
missing `ZAI_API_KEY`, so requiring it would block every merge forever.

**Step 2 — apply the protection.**

```bash
gh api -X PUT repos/{owner}/{repo}/branches/main/protection \
  -H "Accept: application/vnd.github+json" \
  --input - <<'JSON'
{
  "required_status_checks": {
    "strict": true,
    "contexts": [
      "format",
      "build-test (ubuntu-latest)",
      "build-test (windows-latest)",
      "build-test (macos-latest)",
      "asan",
      "perf-gates"
    ]
  },
  "required_pull_request_reviews": null,
  "enforce_admins": false,
  "restrictions": null,
  "required_linear_history": true,
  "allow_force_pushes": false,
  "allow_deletions": false,
  "required_conversation_resolution": false
}
JSON
```

`{owner}/{repo}` are placeholders `gh` fills in from the current repository —
leave them literal. The API requires all four of `required_status_checks`,
`required_pull_request_reviews`, `enforce_admins`, and `restrictions` to be
present; three of them are `null` here on purpose:

| Field | Value | Why |
|---|---|---|
| `required_status_checks.strict` | `true` | the branch must be up to date with `main`, so the six checks ran against what actually merges |
| `required_status_checks.contexts` | the six §3 checks | 16-testing-ci.md §3. They carry no `paths:` filters, so even a docs-only PR reports all six and nothing waits forever |
| `required_pull_request_reviews` | `null` | **no human approval is ever required** (R10). The automated review loop (§2) is the review mechanism, and the reviewer cannot approve a PR |
| `enforce_admins` | `false` | the implementor must be able to unblock itself (e.g. after a mistyped context) without a human |
| `restrictions` | `null` | no push allow-list; only milestone branches are ever pushed anyway (§1.1) |
| `required_linear_history` | `true` | every merge is `--squash` (§2.8) |
| `allow_force_pushes` / `allow_deletions` | `false` | matches §1.1 (never force-push) and protects the trunk |
| `required_conversation_resolution` | `false` | §2.5 already mandates a reply to every comment; a bot-authored thread the implementor cannot resolve must never deadlock a merge |

**Step 3 — verify and journal.**

```bash
gh api repos/{owner}/{repo}/branches/main/protection \
  --jq '{contexts: .required_status_checks.contexts,
         strict:   .required_status_checks.strict,
         reviews:  .required_pull_request_reviews,
         admins:   .enforce_admins.enabled}'
```

Expect the six contexts, `strict: true`, `reviews: null`, `admins: false`,
then append a JOURNAL bullet listing the applied contexts. If the `PUT`
fails for lack of admin rights, take the §3.2 fallback row — protection is a
safety net, never a blocker.

## 2. The PR review loop

This is the core autonomy mechanism. Follow it for every PR, no exceptions.

```
create branch → commits → push → open PR (not draft)
      │
      ▼
   CI runs (3 OS) ──red──► fix, push, wait again
      │green
      ▼
   poll for GLM 5.3 review, every 2–3 min, ≤ 20 min,
   extended while a run is still in_progress (hard stop 35 min)
      │                              │
      │review, inline, or            │deadline hit AND no run
      │issue comment posted          │in flight (§2.7 gate)
      ▼                              ▼
   classify EVERY comment      reviewer-unavailable fallback:
   1) actionable-correct         self-review checklist (§2.7)
      → fix in new commit        → merge + JOURNAL note
   2) style vs plan docs
      → reply citing doc, keep code
   3) factually wrong
      → reply with rebuttal + evidence
      │
      ▼
   reply to every comment; if any fixes pushed,
   the workflow re-reviews automatically (synchronize) → poll again
      │
      ▼
   STEADY STATE reached (or cycle cap, §2.6)
      │
      ▼
   gh pr merge --squash --delete-branch
```

### 2.1 Create the branch and push

```bash
git checkout main && git pull
git checkout -b milestone/M05-table-format
# ... commits ...
git push -u origin milestone/M05-table-format
```

Branches are pushed to the main repository, never from a fork (the review
workflow refuses fork branches). Never open the PR as a draft — the reviewer
skips drafts.

### 2.2 Open the PR

```bash
gh pr create \
  --title "M05: Table format v1 & Neon Drift greybox" \
  --body-file /tmp/pr-body.md
pr=$(gh pr view --json number --jq .number)
```

PR body template (fill every section; delete none — write "n/a" with a reason
if truly empty):

```markdown
## Milestone

M<NN> — <title> (docs/plan/04-milestones.md §M<NN>)

## Scope

<3–8 bullets: what this PR adds/changes. Note anything descoped and where it
went (which milestone / JOURNAL entry).>

## Test evidence

<Paste the local `ctest --preset release --output-on-failure` tail: totals,
plus the names of the new tests this PR adds. For perf/latency claims, paste
the measured numbers.>

## Screenshots / artifacts

<Required for any visually observable change: attach the demo artifact named
in 04-milestones.md for this milestone (screenshot, overlay capture, replay
file, metrics JSON). Otherwise "n/a — headless change".>

## Checklist

- [ ] CI green: all six required checks (§1.8) — `format`, `build-test` on
      ubuntu/windows/macos, `asan`, `perf-gates`
- [ ] Tests added for all new behavior
- [ ] CHANGELOG.md updated under [Unreleased]
- [ ] docs/JOURNAL.md entry appended
- [ ] No orphan TODOs; spec deviations recorded as ADR in 02-decisions.md
```

### 2.3 Wait for CI

```bash
gh pr checks "$pr" --watch --fail-fast
```

If CI is red: fix, push, repeat. Do not poll for a review while CI is red;
the review may run regardless, but comments against a broken build are
re-checked after the fix anyway.

### 2.4 Poll for the automated review

The reviewer posts within a few minutes of each push event. Note the
workflow's concurrency rule: a new push **cancels** an in-progress review run
and starts a fresh one — so do not push while waiting for a review you intend
to read.

Poll every 2–3 minutes for up to 20 minutes after CI goes green. The reviewer
posts its output in one of **three** places — a formal review, inline review
comments, or a plain issue comment — and the loop must break on any of them,
so all three are counted mechanically. Issue comments have no `commit_id`, so
they are detected by comparing a **baseline count taken before the wait**
against the current count, ignoring comments authored by the PR author (the
implementor's own `gh pr comment` replies must not look like a review) and
ignoring anything older than the head commit (a reply from a previous review
cycle must not look like this cycle's review):

```bash
head_sha=$(gh pr view "$pr" --json headRefOid --jq .headRefOid)
author=$(gh pr view "$pr" --json author --jq .author.login)
title=$(gh pr view "$pr" --json title --jq .title)
pushed_at=$(gh pr view "$pr" --json commits --jq '.commits[-1].committedDate')

# Issue comments the PR author did not write, no older than the head commit.
n_issue() {
  gh api "repos/{owner}/{repo}/issues/$pr/comments" -F per_page=100 \
      --jq "[.[] | select(.user.login != \"$author\"
                          and .created_at > \"$pushed_at\")] | length"
}
# Review runs fire on pull_request_target, so they are attached to the BASE
# ref (main), never to the milestone branch — never filter them by --branch.
review_state() {
  gh run list --workflow zai-code-review.yml --event pull_request_target \
      --limit 5 --json status,conclusion,displayTitle,url \
      --jq "[.[] | select(.displayTitle == \"$title\")][0]
            | if . == null then \"missing/none\"
              else \"\(.status)/\(.conclusion // \"none\")\" end"
}
review_running() {
  case "$(review_state)" in
    queued/*|requested/*|waiting/*|pending/*|in_progress/*) return 0 ;;
    *) return 1 ;;
  esac
}

n_issue_base=$(n_issue)                 # baseline BEFORE the wait
found=0
# A non-zero baseline means the reviewer already posted while CI was running:
# that is output, not noise — skip the wait and read it.
if [ "$n_issue_base" -gt 0 ]; then found=1; fi

# True as soon as reviewer output exists in ANY of the three places.
output_posted() {
  # 1) formal reviews (summary + verdict)
  n_reviews=$(gh api "repos/{owner}/{repo}/pulls/$pr/reviews" \
      --jq "[.[] | select(.commit_id == \"$head_sha\")] | length")
  # 2) inline review comments
  n_inline=$(gh api "repos/{owner}/{repo}/pulls/$pr/comments" \
      --jq "[.[] | select(.commit_id == \"$head_sha\")] | length")
  # 3) plain issue comments (some reviewer output lands here)
  n_new_issue=$(( $(n_issue) - n_issue_base ))
  [ "$n_reviews" -gt 0 ] || [ "$n_inline" -gt 0 ] || [ "$n_new_issue" -gt 0 ]
}

deadline=$(( $(date +%s) + 1200 ))      # 20 min nominal
hard_stop=$(( $(date +%s) + 2100 ))     # 35 min = workflow's 30 min cap + margin
# Extend past the nominal deadline while a run is still executing (§2.7).
while [ "$found" -eq 0 ] \
      && { [ "$(date +%s)" -lt "$deadline" ] || review_running; } \
      && [ "$(date +%s)" -lt "$hard_stop" ]; do
  if output_posted; then found=1; break; fi
  echo "review run: $(review_state)"    # did it run, skip, or fail?
  sleep 150
done
# One final read: a run that concluded during the last sleep may have posted
# just as the loop condition went false.
if [ "$found" -eq 0 ] && output_posted; then found=1; fi
```

`found=1` means output exists: read all three sources (`gh pr view "$pr"
--comments` for the issue-comment text) and go to §2.5. `found=0` means the
loop ran out of time — do **not** go straight to the fallback; apply the §2.7
run-status gate first.

If `commit_id` filtering misses comments (the API sometimes reports
`original_commit_id`), fall back to filtering by `created_at` later than the
last push timestamp. If `review_state` reports `completed/skipped`, or the run
shows the "Skipping Z.ai review because ZAI_API_KEY is not configured." step,
stop polling immediately and go to the reviewer-unavailable fallback (§2.7).

### 2.5 Classify and answer every comment

Read every review comment — inline, review summary, and plain issue comments
(all three surfaces of §2.4). Classify each into exactly
one bucket and act:

| # | Bucket | Test | Action |
|---|---|---|---|
| 1 | Actionable-correct | The comment identifies a real defect, spec violation, missing test, or genuinely better approach not contradicted by the plan docs | Fix it in a **new commit** (`fix(<scope>): address review — <summary>`), then reply linking the commit hash |
| 2 | Style/preference conflicting with plan docs | The suggestion contradicts PLAN.md canon, this document, or a spec doc (naming, structure, error policy, physics constants, …) | Do **not** change the code. Reply citing the doc and section: `Declining: mandated by docs/plan/03-process.md §1.3 (members use m_ prefix). Keeping as specified.` |
| 3 | Factually wrong | The comment misreads the code, invents an API, or asserts false behavior | Reply with a reasoned rebuttal and evidence: quote the code path, the test that covers it, or the reference (C++ standard, SDL3 docs, spec doc section) |

Rules:

- **Reply to every comment.** Zero unanswered comments at merge time.
- Inline comment replies:
  `gh api -X POST "repos/{owner}/{repo}/pulls/$pr/comments/<comment_id>/replies" -f body="..."`.
  Summary/issue comments: `gh pr comment "$pr" --body "..."`.
- Never argue taste. A bucket-2 reply cites a document or it becomes
  bucket 1.
- If a comment is correct about a defect in the **spec**, apply the
  spec-change protocol (§3.3) in the same PR and reply pointing at the spec
  diff and new ADR.
- After pushing fix commits, the workflow re-reviews automatically on
  `synchronize` — return to §2.3 (CI) then §2.4 (poll).

### 2.6 Steady state and the cycle cap

**Steady state** is: a full review cycle — push, workflow run completes,
every resulting comment classified — that produces **zero new actionable
items** (zero bucket-1 items). A point the reviewer repeats that has already
been rebutted **twice** in this PR (buckets 2 or 3, with replies) counts as
non-new and does not block steady state.

**Cycle cap:** after **5** full review cycles without steady state:

1. Fix all remaining bucket-1 correctness items unconditionally (correctness
   is never negotiable).
2. Document remaining style disagreements in a final PR comment and in
   `docs/JOURNAL.md`.
3. Merge.

### 2.7 Reviewer-unavailable fallback

**Run-status gate — check this before declaring anything.** The 20-minute
poll window is shorter than the review workflow's own `timeout-minutes: 30`,
so "nothing posted yet" does not mean "no reviewer". Read `review_state`
(§2.4) and act on `status` first:

| `review_state` | Meaning | Action |
|---|---|---|
| `queued/*`, `in_progress/*` | a run is still executing | **Keep waiting.** The workflow caps itself at 30 minutes, so this is bounded; re-poll §2.4 until it reaches a conclusion, then re-read this table |
| `completed/success` with no review, inline, or issue comment | ran, produced nothing | fallback below |
| `completed/failure`, `completed/cancelled`, `completed/timed_out` | workflow failure, API outage, cancelled by a push | §3.2 "Review bot rate-limited / errored" retry first, then fallback below |
| `completed/skipped` | drafts / fork branch / missing `ZAI_API_KEY` | fallback below immediately, no retry |
| `missing/none` | no run was ever created for this PR | fallback below |

The honest worst-case wait is therefore **~35 minutes** from green CI (30-min
workflow cap plus a polling margin), not 20 — the 20 minutes is only the
nominal window before the run-status gate takes over. PLAN.md §7's "~20 min"
is that nominal figure.

Once the gate says the reviewer produced no output for this head SHA (missing
`ZAI_API_KEY`, workflow failure, API outage, run concluded empty — never
while a run is still `in_progress`), and one rate-limit retry (§3.2 fallback
matrix) has been exhausted, run the **self-review checklist**: re-read the
complete diff with `gh pr diff "$pr"`, hunk by hunk, asking for each hunk:

1. **Correctness.** Off-by-one; degrees passed where radians are expected
   (canon: SI internally, `_deg` only in JSON); sign errors against the
   bottom-left-origin +y-up-table coordinate system; ticks confused with
   seconds; uninitialized members; iterator invalidation; overflow on
   `uint32` tick/score counters; float comparison without tolerance.
2. **Spec conformance.** Name the spec doc and section this hunk implements.
   A hunk that implements no cited section is scaffolding or scope creep —
   delete it or justify it in the PR body.
3. **Tests.** Every new public behavior has a test that would fail if the
   hunk were reverted. If not, add it before merging.
4. **Determinism.** No wall-clock reads in sim; no iteration over
   pointer-keyed or `unordered_*` containers affecting sim results; no
   frame-rate-dependent terms; all randomness via the seeded PCG32.
5. **Latency rules.** Playfield frames-in-flight stays 1; late-latch
   preserved; nothing blocking, allocating, throwing, or logging added to the
   sim tick or audio callback.
6. **Naming and conventions.** §1.3 table, include order, clang-format clean,
   TODO discipline.

Fix what the checklist finds, push, wait for CI green, then merge with a
JOURNAL note naming the concluded run state that justified the fallback:
`Merged under reviewer-unavailable fallback (review run completed/skipped —
no ZAI_API_KEY); self-review completed.` **Never merge a known correctness bug, even if the reviewer is
silent.** A known bug either blocks the merge or is fixed first.

### 2.8 Merge

Preconditions: CI green on all 3 OS **and** steady state reached (or the
cycle-cap procedure of §2.6 / fallback of §2.7 completed) **and** the per-PR
checklist (§4) fully checked.

```bash
gh pr merge "$pr" --squash --delete-branch
```

The squash commit title is the PR title (`M<NN>: <title>`). Then
`git checkout main && git pull` and begin the next milestone. Never start
milestone N+1 before milestone N is merged (PLAN.md §2).

## 3. Autonomy protocol

### 3.1 docs/JOURNAL.md

Append-only development journal. Seeded at M0; every PR appends. **Never edit
or delete a past entry** — corrections are new entries. Entry format, one per
milestone (or per split part), appended at the bottom:

```markdown
## M05 — Table format v1 & Neon Drift greybox (2026-09-12)

- Shipped: table.json loader, prefab expansion, test-lab, neon-drift greybox.
- Deviations: plunger max pull force raised 20→24 N (spec fixed in
  09-table-format.md same PR).
- New ADRs: ADR-017 (prefab expansion happens at load, not in the validator).
- Worries: arc–arc corner CCD has a hand-tuned epsilon; revisit if M8 ramps
  jitter.
```

Mid-milestone notable events (quarantined test, fallback taken, reviewer
unavailable) get their own dated bullet appended under the current milestone
heading. The seed content is specified in 04-milestones.md §M0.

### 3.2 Fallback matrix

Every foreseeable blocking situation has a documented, human-free fallback.
When a fallback triggers, execute it, append a JOURNAL bullet, and continue.
Never wait for a human.

| Situation | How you detect it | Fallback action |
|---|---|---|
| vcpkg port missing or broken on one OS | `vcpkg install` / configure fails in CI or locally | Pin the dependency via CMake `FetchContent` with a **fixed release tag** (never a branch); record the tag and rationale as an ADR in 02-decisions.md |
| SDL_shadercross fails to build or run | Shader build step fails on any OS | Commit precompiled shader blobs under `/shaders/compiled/` (SPIR-V + DXIL + MSL), generated on a working platform, per ADR-012 in 02-decisions.md; keep HLSL source authoritative and regenerate when the toolchain recovers |
| GPU unavailable in CI | GPU device creation fails on a runner | Renderer smoke tests call `GTEST_SKIP()` with a logged warning (`TB_LOG_WARN("no GPU; skipping render smoke tests")`); sim/headless tests always run and must pass |
| macOS raw input path impractical | IOKit HID work exceeds scope | Use the SDL event fallback on macOS — this is already canon (PLAN.md §5.4); note measured latency delta in JOURNAL |
| Flaky test (passes on rerun, fails intermittently) | Same test flips outcome without code change | CI already retries once (`--repeat until-pass:2`); a pass on attempt 2 is a **flake occurrence** — record it in `docs/JOURNAL.md` (test name, job, run link) before merging. On a **second occurrence of the same test within 30 days**, quarantine per 16-testing-ci.md §6: append the exact `suite.case` name (one per line) to `tests/quarantine.txt` — never edit the test source, no `GTEST_SKIP()`; CMake reads the file at configure time and sets the `DISABLED` property, and `weekly-deep` keeps running it informationally. The JOURNAL entry states the suspected cause and the milestone by which it will be fixed; deflake (and remove the line) within two milestones or delete the test with a JOURNAL justification. More than 5 entries in `quarantine.txt` blocks further quarantines — fix the flakiest first. Determinism (`det_`) tests never retry and never quarantine — a flaky one is a live correctness bug, fix it |
| Reference hardware (Profile A cabinet) unavailable | A milestone's acceptance criteria require the physical cabinet (08-physics.md §9 perf figures, 12-audio.md §12 latency gate, 07-displays.md Done-when display cases, the hardware-bound M20 audit rows) and the implementor does not have it | Run the same instrumented protocol unchanged on whatever machine is available, record the measured **deltas** against the specified targets plus the **machine spec** in `docs/JOURNAL.md`, and tag those acceptance rows as **measured-on-non-reference** (PROVISIONAL-PASS in the M20 audit, per the 01-product.md §3 hardware-fallback rule); a tagged row satisfies its milestone's acceptance criteria. **Never block a merge on hardware the implementor does not have** (R10) — and never weaken the documented target itself, which stays the number to confirm on the cabinet |
| Perf gate failure | CI perf job red | Profile (per 16-testing-ci.md tooling) and fix the regression. **Never weaken, delete, or skip the gate** |
| Review bot rate-limited / errored | `review_state` (§2.4) reports `completed/failure` or `completed/timed_out`, or the review is truncated with an API error. A `queued`/`in_progress` run is **not** a failure — wait it out per the §2.7 run-status gate | Wait 5 minutes, retrigger once with an empty commit (`git commit --allow-empty -m "chore: retrigger review" && git push`); if it fails again, apply §2.7 reviewer-unavailable fallback |
| Branch protection cannot be configured | The §1.8 `PUT` returns 403/404 — no admin rights on the repository, or the endpoint is unavailable | Proceed **without** protection; it is a safety net, not the enforcing mechanism. The enforcing mechanism is then the per-PR checklist (§4): `gh pr checks "$pr" --watch --fail-fast` (§2.3) must show all six §1.8 contexts green before every `gh pr merge`, and a red check blocks the merge exactly as protection would. Record the exact error, the date, and this decision in `docs/JOURNAL.md`; retry the `PUT` once at the next milestone in case rights changed. **Never block a milestone on it** |
| Pinned third-party asset undownloadable (vendored OFL font, etc.) | The fetch of the pinned `github.com/google/fonts` commit fails (offline runner, path gone at that commit) or the file's committed SHA-256 does not match | Substitute any available OFL face of the same class — Orbitron → geometric square sans, Monoton → inline/neon display, Righteous → rounded geometric display — copy it into `/assets/fonts/` with its `OFL.txt`, update that file's committed SHA-256, and keep the logical names (`orbitron`, `monoton`, `righteous`) so `art.json` and 13-art-direction.md §5 stay valid. Record the substitution **and its visual delta** (which §5 typography rules the substitute breaks: cap height, minimum rendered size, letter-spacing) as an ADR in 02-decisions.md plus a JOURNAL bullet. **Never block a milestone on a download** — fonts are acquired at M0 and only consumed from M13 |
| A test, tool run, or the game appears to hang | A CI job hits the workflow timeout, `ctest` stops producing output, or `tb_autoplay` stops advancing ticks | It is never the sim waiting on a captured ball: on `tilt` (and on Duel timeout) every captured ball — kickers including script holds, and ball locks — is force-ejected at its element default (11-game-framework.md §5), an unclaimed lock auto-releases after 3,000 ms (08-physics.md §6.14), and if no ball is free and none is in the plunger lane for 30,000 ticks the framework runs a ball search that **does** eject kickers and locks, logs an error, and plays on (11-game-framework.md §4.6). Treat the hang as a harness bug: a wall-clock `sleep`/poll in a test, an unbounded `while` around `step()` waiting for a condition, or a blocking read. Fix the harness — never add a sleep, never raise the job timeout to hide it |

### 3.3 Spec-change protocol

Specs are binding, but implementation reality wins over a wrong spec. When
implementation reveals a spec error (wrong constant, impossible geometry,
missing case, contradictory requirement):

1. Fix the spec document **in the same PR** as the code (smallest edit that
   makes the spec true and unambiguous).
2. Add an ADR to 02-decisions.md: number sequentially, state context, the
   old spec text, the new behavior, and why.
3. Append a JOURNAL "Deviations" bullet.
4. If the error is in PLAN.md §5 canon itself, do **not** silently diverge:
   the ADR explicitly amends canon (PLAN.md §5 allows this) and the PR
   updates the losing document per PLAN.md §5.10.

## 4. Per-PR definition of done

Check every box before `gh pr merge`. Any unchecked box blocks merge.

- [ ] Scope matches the 04-milestones.md entry: everything "Scope in" is
      done, nothing "Scope out" leaked in.
- [ ] CI green: all six §1.8 required checks reported and passing —
      `format`, `build-test (ubuntu-latest)`, `build-test (windows-latest)`,
      `build-test (macos-latest)`, `asan`, `perf-gates`. This checklist item
      is what enforces the merge gate whenever branch protection could not be
      configured (§3.2).
- [ ] Every new behavior has a test; `tb_tests` passes locally and in CI;
      determinism suite green (from M2 onward).
- [ ] Milestone acceptance criteria in 04-milestones.md all satisfied (final
      part of a split satisfies the whole milestone's criteria).
- [ ] `clang-format` clean (CI-enforced) and naming per §1.3.
- [ ] Sim hot path still exception-free and allocation-free (allocation-hook
      test green).
- [ ] CHANGELOG.md updated under `[Unreleased]`.
- [ ] docs/JOURNAL.md entry appended.
- [ ] Spec deviations: spec doc fixed + ADR added, or none occurred.
- [ ] No orphan TODOs (§1.7).
- [ ] Demo artifact from 04-milestones.md attached to the PR body.
- [ ] Review loop completed: steady state, cycle cap procedure, or
      reviewer-unavailable fallback — with every comment answered.
- [ ] No known correctness bug is being merged.

## 5. Milestone splitting

A milestone may be split when its projected size exceeds **~3,000 added
lines** (count: `git diff main --numstat`, summing added lines, excluding
vendored code, generated files, lockfiles, and committed binary blobs;
table-pack JSON/Lua content counts).

Rules:

- Split into consecutive PRs `M<NN>a`, `M<NN>b` (rarely `c`), branches
  `milestone/M<NN>a-<slug>`, merged in order; part b starts only after part a
  merges.
- Decide the split **before starting**, at a stable interface boundary
  (e.g. M13a = art/particle/bloom engine, M13b = Neon Drift art content).
  Never split mid-feature.
- Every part independently: builds green, tests pass, full review loop, its
  own CHANGELOG and JOURNAL entries.
- The milestone's acceptance criteria are checked in the final part's PR;
  earlier parts state which criteria they intentionally leave open.
- Pre-authorized splits are marked in 04-milestones.md (M13, M17).

## Common pitfalls

- **Merging on green CI without the review loop.** CI green is necessary,
  not sufficient. Poll for the review (§2.4), answer every comment, reach
  steady state or a documented fallback — then merge.
- **Opening the PR as a draft and waiting forever.** The review workflow
  skips drafts. Always open non-draft; convert nothing to draft.
- **Pushing while a review run is in progress.** The workflow's concurrency
  group cancels the running review and you lose it. Batch fixes, then push
  once, then poll.
- **Polling only for formal reviews and inline comments.** The reviewer may
  post its whole summary as a plain **issue** comment, which carries no
  `commit_id` and appears in neither `pulls/$pr/reviews` nor
  `pulls/$pr/comments`. Count it against a pre-wait baseline (§2.4) and break
  on any of the three surfaces; eyeballing `gh pr view --comments` inside a
  non-interactive loop detects nothing and ends in a false "reviewer
  unavailable".
- **Filtering review runs with `--branch`.** The review workflow triggers on
  `pull_request_target`, so its runs are associated with the **base** ref, not
  `milestone/M<NN>-<slug>`; a `--branch` filter can match nothing forever. Use
  `--event pull_request_target` and select the run by PR title or number
  (§2.4 `review_state`).
- **Declaring the reviewer unavailable while its run is still executing.**
  The 20-minute poll window is shorter than the workflow's
  `timeout-minutes: 30`. Apply the §2.7 run-status gate: a `queued` or
  `in_progress` run means keep waiting (bounded by that 30-minute cap, ~35
  minutes worst case), never self-merge out from under a slow-but-healthy
  review.
- **Treating every review comment as an order.** Bucket-2 comments that
  contradict the plan docs must be declined with a citation. Changing
  spec-mandated behavior to satisfy a reviewer breaks the build sequence for
  later milestones.
- **Rebutting a correct comment.** If the reviewer found a real bug, bucket 1
  applies even if the fix is annoying. When in doubt between bucket 1 and 3,
  write the test that would settle it; the test decides.
- **Force-pushing during review.** It orphans inline comment anchors and
  makes reply threading fail. Append commits; squash happens at merge.
- **Editing JOURNAL history or back-dating entries.** Append-only. A wrong
  entry is corrected by a new entry.
- **Putting side effects in `TB_ASSERT`.** The expression is not evaluated
  in release builds. `TB_ASSERT(queue.pop())` silently changes behavior
  between configurations; hoist the call, assert the result.
- **"Fixing" a red perf gate by raising the threshold or skipping the
  test.** Forbidden by the fallback matrix. Profile and fix the regression;
  gates only move via the tuning procedure in 16-testing-ci.md plus an ADR.
- **Quarantining a test by editing its source.** Quarantine is one
  `suite.case` line in `tests/quarantine.txt` (16-testing-ci.md §6), which
  CMake turns into the `DISABLED` property; a hand-written `GTEST_SKIP()`
  hides the test from `weekly-deep` too and leaves nothing to grep at
  deflake time.
- **Quarantining a determinism failure as "flaky".** Determinism tests are
  never flaky; a nondeterministic sim is a correctness bug (canon §5.3).
  Find the wall clock read, unordered iteration, or uninitialized value.
- **Typing a required check name from prose instead of from `gh pr checks`.**
  A context that never reports leaves every future PR stuck on "Expected —
  Waiting for status to be reported" forever. Copy the names verbatim
  (§1.8 step 1) and read them back (step 3); if one is already wrong, re-run
  the `PUT` with the corrected list — `enforce_admins: false` is exactly what
  lets you unblock yourself without a human.
- **Requiring a human review on `main`.** `required_pull_request_reviews`
  stays `null`: the automated reviewer cannot approve a PR, so any approval
  requirement is a permanent wait for a human, which R10 forbids. Same for
  making `zai-code-review` a required context — it self-skips on drafts and
  on a missing secret.
- **Blocking a milestone on repo admin rights or on a download.** Both have
  fallback rows (§3.2): ship without branch protection and let the §4
  CI-green checklist enforce merges, or substitute an available OFL face and
  record the ADR + JOURNAL entry. Waiting for either is waiting for a human.
- **Calling a stalled job a "sim hang" and raising the timeout.** The sim
  cannot deadlock on a held ball — tilt force-ejects captures, unclaimed
  locks auto-release at 3,000 ms, and the 30,000-tick ball search ejects
  kickers and locks (§3.2 row). A stall is a harness bug; find the sleep,
  poll, or unbounded loop.
- **Stalling a milestone on hardware you do not have.** The reference
  cabinet is fallback-eligible (§3.2): measure on the machine you have, tag
  the row measured-on-non-reference, journal the delta, merge. Waiting for
  hardware is waiting for a human, which R10 forbids.
- **Leaving reviewer comments unanswered because they seem minor.** Every
  comment gets a reply (fix link, citation, or rebuttal). Steady-state
  accounting depends on it.
- **Starting milestone N+1 while N's PR is still open.** One milestone in
  flight at a time, always merged in order.

## Done when

- [ ] `/.clang-format` exists with exactly the §1.4 content, and CI enforces
      it with clang-format 18.
- [ ] `CHANGELOG.md` and `docs/JOURNAL.md` exist with the seed formats and
      grow with every merged PR.
- [ ] `src/core/assert.h` implements `TB_ASSERT`/`TB_CHECK` exactly as §1.6,
      with `TB_DEBUG` defined only in Debug configuration.
- [ ] Every merged PR: branch named `milestone/M<NN>-<slug>`, title
      `M<NN>: <title>`, conventional commits, squash-merged, branch deleted.
- [ ] Every merged PR body follows the §2.2 template with test evidence and
      demo artifact.
- [ ] Every review comment on every merged PR has a reply (fix, citation, or
      rebuttal) — including reviewer output posted as a plain issue comment;
      no PR merged before steady state, cycle cap, or the documented fallback.
- [ ] No reviewer-unavailable fallback was ever taken while a review run was
      still `queued` or `in_progress`; each one names the concluded
      `review_state` (§2.7 gate) in its JOURNAL bullet.
- [ ] `main` carries the §1.8 protection from M0 on: exactly the six
      16-testing-ci.md §3 contexts, `strict: true`,
      `required_pull_request_reviews: null`, `enforce_admins: false`,
      `restrictions: null` — verified by the step-3 read-back and journaled.
      If the API refused, a JOURNAL entry records the failure and every
      merged PR still shows those six checks green (§3.2 fallback).
- [ ] Every vendored third-party asset under `/assets` is either the pinned
      upstream file matching its committed SHA-256, or a documented
      substitution with an ADR and a JOURNAL entry; no milestone ever waited
      on a download.
- [ ] Zero occurrences in the repo of untagged TODO/FIXME/XXX/HACK.
- [ ] All fallback-matrix activations are traceable to JOURNAL entries (and
      ADRs where the matrix requires one).
- [ ] Every quarantined test is a `suite.case` line in
      `tests/quarantine.txt` (≤ 5 entries) with its JOURNAL entry; no
      committed test carries a hand-written `GTEST_SKIP()` quarantine and no
      `det_` test is quarantined.
- [ ] Every acceptance row verified away from the Profile A cabinet is
      tagged measured-on-non-reference, with the deltas and the machine spec
      in `docs/JOURNAL.md`; no merge was ever blocked waiting on hardware.
- [ ] The sim hot path allocation/exception rules hold (allocation-hook test
      exists and is green from M2 onward).
- [ ] A fresh LLM session given only PLAN.md and this document can execute
      the full PR loop for a trivial change without asking a human anything.
