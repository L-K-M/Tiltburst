# 07 — Displays

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 05-engine-core.md (main loop, settings, crash-safe writes),
06-rendering.md (renderer, overlay drawing). Implements requirement R4 and
canon §5.9; primary milestone M12 (window/loop skeleton parts land in M1).

This document owns: display enumeration, the auto-detection algorithm,
`displays.json`, rotation policy and the projection math, window and
swapchain creation, backglass pacing, hotplug, single-display fallback,
and windowed dev mode. Threading is canon §5.4: both windows are rendered
from the **main** thread; there is no render thread. ARCHITECTURE.md
ADR-004's separate backglass thread and frames-in-flight 2 are superseded
by canon (PLAN §5.10).

## 1. Terms

- **Role**: `playfield` or `backglass`. v1 has exactly these two windows
  (DMD/topper is out of scope, designed-for via the same role table).
- **Reported size**: pixel size of a display's desktop mode as the OS
  reports it. A physically rotated TV usually still reports landscape.
- **Squareness**: `min(w,h) / max(w,h)` ∈ (0,1]; 1.0 = square.
- **Rotation**: degrees the rendered content is rotated counter-clockwise
  in the projection (§6). Never OS rotation (canon §5.9).

## 2. Enumeration

```cpp
int n = 0;
SDL_DisplayID* ids = SDL_GetDisplays(&n);          // caller frees with SDL_free
for (int i = 0; i < n; ++i) {
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i]);  // NOT Current
    DisplayInfo d;
    d.index         = i;                            // position in SDL order
    d.name          = SDL_GetDisplayName(ids[i]);   // e.g. "SAMSUNG 32in TV"
    d.w             = m->w;  d.h = m->h;            // pixels
    d.refresh_hz    = m->refresh_rate;              // 0.0f if unknown -> treat as 60
    d.content_scale = SDL_GetDisplayContentScale(ids[i]);
    d.sdl_id        = ids[i];
}
```

`DisplayInfo` is a plain struct (no SDL types) declared in
`src/platform/display_detect.h` so detection is a pure, CI-testable
function (§12). Detection uses pixel sizes only; `content_scale` is used
solely by 06-rendering.md for overlay text sizing. Aspect ratios are
unaffected by uniform DPI scaling, so detection is DPI-robust.

## 3. Detection algorithm (expands canon §5.9 — binding)

```
struct RoleConfig  { string match = "auto";      // "auto" | "name:<glob>" | "index:<n>"
                     string rotation = "auto";   // "auto" | "0"|"90"|"180"|"270"
                     bool enabled = true; }      // backglass only
struct Assignment  { int playfield = -1;         // index into the input list
                     int backglass = -1;         // -1 = none
                     int pf_rotation = 0;        // resolved degrees CCW
                     int bg_rotation = 0;
                     vector<string> warnings; }

Assignment detect(const vector<DisplayInfo>& ds, const DisplaysConfig& cfg):
    A = Assignment{}
    if ds.empty(): return A                      # caller: fatal unless --headless

    # --- 1. explicit config beats heuristics, per role independently ---
    pf = resolve_match(cfg.playfield.match, ds)  # -1 for "auto" or no match
    bg = cfg.backglass.enabled ? resolve_match(cfg.backglass.match, ds) : DISABLED
    if cfg.playfield.match != "auto" and pf == -1:
        A.warnings += "playfield match '<...>' not found; using heuristics"
    (same for backglass)
    if pf != -1 and pf == bg:
        A.warnings += "playfield and backglass match the same display; backglass dropped"
        bg = -1

    # --- 2. stability: reuse last auto assignment when nothing changed ---
    if pf == -1 and bg == AUTO_UNSET and cfg.last_auto is set
       and set(names(ds)) == set(cfg.last_auto.names)
       and all last_auto names resolve uniquely:
        return assignment from last_auto (rotations via step 4)

    # --- 3. heuristic scoring ---
    portrait  = [d in ds if (float)d.h / d.w >= 1.4]      # canon 5.9
    landscape = [d in ds if (float)d.w / d.h >= 1.4]
    if pf == -1:
        pool = portrait  if portrait  not empty
          else landscape if landscape not empty
          else ds
        # "largest wins": lexicographic key, higher is better
        pf = argmax over pool of key (w*h, refresh_hz, -index)
    if bg is AUTO_UNSET:
        rest = ds minus pf
        if rest not empty:
            bg = argmax over rest of key (squareness, w*h, -index)
        else: bg = -1

    # --- 4. rotation resolution ---
    A.pf_rotation = cfg.playfield.rotation != "auto" ? int(cfg.playfield.rotation)
                    : auto_pf_rotation(ds[pf], bg, ds)
    A.bg_rotation = cfg.backglass.rotation != "auto" ? int(cfg.backglass.rotation) : 0
    A.playfield = pf; A.backglass = bg
    return A

int auto_pf_rotation(DisplayInfo pf, int bg, ds):
    if pf.h >= pf.w:            return 0     # reported portrait: already upright
    # reported landscape. Cabinet (physically rotated TV) vs desktop monitor:
    # a cabinet has a near-square backglass display (canon reference hardware).
    if bg != -1 and squareness(ds[bg]) >= 0.70:
        return 90                            # cabinet assumption; flip -> 270 via config
    return 0                                 # desktop: render portrait, pillarboxed
```

`resolve_match`: `"index:<n>"` = 0-based position in `SDL_GetDisplays`
order; `"name:<glob>"` = case-sensitive glob (`*` and `?` only) against
the display name, lowest index wins on multiple matches (duplicate-name
monitors are common — warn when a name glob is ambiguous). All tie-breaks
above are exact lexicographic comparisons; given the same input list the
function is fully deterministic.

The 0.70 squareness threshold: 5:4 (0.80), 4:3 (0.75) and square panels —
typical cabinet backglass displays — pass; 16:9 (0.5625) and 16:10
(0.625) — typical desktop second monitors — fail. If a real cabinet uses
a 16:9 backglass, the user sets `"rotation": 90` explicitly (§5); the
settings UI (M18, 11-game-framework.md) exposes exactly that switch.

## 4. Worked examples (these are unit tests T1–T4, §12)

**(1) Reference cabinet** — physically rotated portrait TV + 5:4 backglass.
Input: `[{“SAMSUNG TV”, 1920×1080, 60}, {“NEC 1280”, 1280×1024, 60}]`.
No display has h/w ≥ 1.4. Landscape pool = {TV} (1.78 ≥ 1.4; the NEC is
1.25). Playfield = TV. Rest = {NEC}, squareness 0.80 → backglass.
Rotation: playfield reported landscape, backglass squareness 0.80 ≥ 0.70 →
**playfield = TV rotated 90°, backglass = NEC rotation 0**.

**(2) True portrait + 4:3.**
Input: `[{1080×1920, 60}, {1024×768, 60}]`. Portrait pool = {1080×1920}
(h/w 1.78) → playfield, rotation 0 (reported portrait). Backglass =
1024×768 (squareness 0.75). **No projection rotation anywhere.**

**(3) Desktop, single 16:9** — `[{2560×1440, 144}]`. No portrait; landscape
pool = {it} → playfield. No remaining display → no backglass. Rotation:
landscape but no square backglass → 0. **Playfield only, pillarboxed
portrait table** (fit: scale = min(2560/0.52, 1440/1.04) = 1384.6 px/m,
table uses 720×1440 px, 920 px black pillars each side), backglass content
available as the B-key overlay (§10).

**(4) Laptop + external 16:9.**
Input: `[{“Laptop”, 1920×1080, 60}, {“U2719”, 2560×1440, 60}]`. Landscape
pool = both; largest area wins → playfield = U2719. Backglass = laptop
(only one left, squareness 0.5625). Rotation: backglass squareness < 0.70
→ **rotation 0**: pillarboxed upright table on the external, scores on the
laptop panel. (A desktop dev setup must never get a sideways table.)

## 5. `displays.json`

Location: `<pref>/displays.json`, or the `--display-config <path>`
override (05-engine-core.md §2). Written with the crash-safe procedure of
05-engine-core.md §11.2. The engine writes `last_auto` (and only
`last_auto`) after every successful auto-detection — **never** when
`--display-config` supplied the file (user files are read-only to us).

```jsonc
{
  "version": 1,
  "playfield": {
    "match": "auto",           // "auto" | "name:<glob>" | "index:<n>"
    "rotation": "auto"         // "auto" | 0 | 90 | 180 | 270  (CCW, §6)
  },
  "backglass": {
    "match": "auto",
    "rotation": "auto",
    "enabled": true            // false: never create a backglass window
  },
  // Engine-written record of the last heuristic result. Used only to keep
  // assignments stable across restarts when the display set is unchanged.
  // Not user intent; users edit "match"/"rotation" above instead.
  "last_auto": { "playfield": "SAMSUNG TV", "backglass": "NEC 1280" }
}
```

Explicit (non-`"auto"`) values always beat heuristics (canon §5.9). A
match that resolves to nothing falls back to heuristics for that role and
logs the §3 warning. Missing file ⇒ all-`"auto"` defaults.

## 6. Rotation policy and projection math (binding)

Canon: when the playfield display reports landscape but is physically a
rotated portrait TV, rotate **purely in the projection matrix**. Never
request OS display rotation, never rotate via a compositor, never swap
swapchain extents. Everything player-visible on that window — table, HUD,
overlays (05-engine-core.md §14) — renders through the rotated projection
so it reads upright to the player.

### 6.1 Fit (letterbox/pillarbox) for a W×H-meter table in any viewport

Table play area is `table_w × table_h` meters (default 0.52 × 1.04, canon
§5.3; per-table override from 09-table-format.md).

```
struct Fit { float px_per_m; float l, r, b, t; };   // ortho bounds, meters

Fit fit(float table_w, float table_h, int view_w_px, int view_h_px, int rot) {
    bool swap  = (rot == 90 || rot == 270);
    float ew   = swap ? view_h_px : view_w_px;      // effective viewport
    float eh   = swap ? view_w_px : view_h_px;
    float s    = min(ew / table_w, eh / table_h);   // px per meter
    float mx   = (ew - s * table_w) * 0.5f / s;     // margins, meters
    float my   = (eh - s * table_h) * 0.5f / s;
    return { s, -mx, table_w + mx, -my, table_h + my };
}
```

Margins become black bars (cleared every frame, 06-rendering.md).

### 6.2 Matrix construction

Column-vector convention, y-up NDC `[-1,1]²`; 06-rendering.md applies the
backend's NDC y-convention as its own final step.

```
Ortho(l,r,b,t) = | 2/(r-l)   0        0   -(r+l)/(r-l) |
                 | 0         2/(t-b)  0   -(t+b)/(t-b) |
                 | 0         0        1    0           |
                 | 0         0        0    1           |

Rz(0)   = I
Rz(90)  = |  0 -1  0  0 |     Rz(180) = | -1  0  0  0 |    Rz(270) = |  0  1  0  0 |
          |  1  0  0  0 |               |  0 -1  0  0 |              | -1  0  0  0 |
          |  0  0  1  0 |               |  0  0  1  0 |              |  0  0  1  0 |
          |  0  0  0  1 |               |  0  0  0  1 |              |  0  0  0  1 |

P = Rz(rotation) * Ortho(fit(table_w, table_h, view_w, view_h, rotation))
```

Rotating NDC onto NDC is aspect-safe because the fit already used the
*effective* (swapped) viewport. Worked numbers, reference cabinet
(1920×1080 viewport, rotation 90): effective 1080×1920; `s =
min(1080/0.52, 1920/1.04) = 1846.2 px/m`; table occupies 960×1920 px;
`mx = 60 px = 0.0325 m`; ortho bounds `l=-0.0325, r=0.5525, b=0, t=1.04`.

### 6.3 Orientation contract

With `Rz(90)` (CCW in NDC), table y=0 (flipper end) lands on the
framebuffer's **right** edge. On a panel that was physically rotated 90°
clockwise (native top edge now at the player's right), the framebuffer's
right edge is the physical bottom — flippers correctly at the bottom:

```
   table space (portrait)          1920x1080 framebuffer (rotation 90)
  y=1.04 +--------------+          +---+--------------------------------+---+
  far    |              |          | b |  y=1.04                  y=0   | b |
  end    |              |   ==>    | a |  far end   <--- +y --- flipper | a |
         |              |          | r |                          end   | r |
  y=0    +--##------##--+          +---+--------------------------------+---+
         flippers                   ^ fb left = physical TOP        ^ fb right
         x=0 ...... x=0.52            (panel turned 90° CW)           = physical BOTTOM
```

If the cabinet's TV was rotated the other way (counter-clockwise), the
table appears upside down; the fix is `"rotation": 270` in `displays.json`
(auto always picks 90; the M18 settings screen offers the flip). This
sentence must appear in the user-facing docs.

## 7. Window creation and swapchains

Fullscreen path (default). Per assigned role, on the target display:

```cpp
const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(display_id);
SDL_PropertiesID p = SDL_CreateProperties();
SDL_SetStringProperty (p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Tiltburst");
SDL_SetNumberProperty (p, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                       SDL_WINDOWPOS_CENTERED_DISPLAY(display_id));
SDL_SetNumberProperty (p, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                       SDL_WINDOWPOS_CENTERED_DISPLAY(display_id));
SDL_SetNumberProperty (p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,  m->w);
SDL_SetNumberProperty (p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, m->h);
SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
SDL_Window* win = SDL_CreateWindowWithProperties(p);
SDL_DestroyProperties(p);
SDL_SetWindowFullscreenMode(win, NULL);   // NULL = BORDERLESS desktop fullscreen
```

Never pass a concrete mode to `SDL_SetWindowFullscreenMode` — exclusive
fullscreen blanks or disturbs the other display on at least one platform
(ARCHITECTURE.md §3 rule 4).

Claim and configure (device already created with frames-in-flight 1,
05-engine-core.md §1 step 9 — `SDL_SetGPUAllowedFramesInFlight` is
**device-wide** in SDL3; the backglass shares it, which is harmless
because its acquire never blocks):

```cpp
SDL_ClaimWindowForGPUDevice(dev, win);
SDL_GPUPresentMode pm;
if (role == playfield) {
    // settings.video.present_mode (05 §11): auto = MAILBOX if available
    pm = (want_mailbox && SDL_WindowSupportsGPUPresentMode(
              dev, win, SDL_GPU_PRESENTMODE_MAILBOX))
         ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
} else {
    pm = SDL_GPU_PRESENTMODE_VSYNC;       // backglass: always VSYNC
}
SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
```

Acquire semantics (used by the 05-engine-core.md §5 main loop):

| Window | Call | Behavior |
|---|---|---|
| Playfield | `SDL_WaitAndAcquireGPUSwapchainTexture` | Blocks until a swapchain image is available, honoring frames-in-flight 1. This is the frame pacer in VSYNC mode. |
| Backglass | `SDL_AcquireGPUSwapchainTexture` | Non-blocking: returns `true` with `*texture == NULL` when not ready → **skip the backglass frame**. `false` = error → log `warn`, skip. |

Both paths must end their command buffer exactly once: submit it after
drawing, or `SDL_CancelGPUCommandBuffer` it when no texture was acquired
(06-rendering.md §4.1/§4.2, 05-engine-core.md §5). An acquired command
buffer is never abandoned — and never both submitted and cancelled.

## 8. Backglass pacing

Canon §5.4: backglass renders from the **main** thread at a ~30 Hz cadence
with the non-blocking acquire, so it can never stall the playfield. The
cadence logic lives in the main loop (05-engine-core.md §5): a deadline
`next_backglass_ns` advanced by 33 333 333 ns per drawn frame, resynced to
`now` when more than 100 ms behind (after a hitch), and **not** advanced
when the acquire yields no texture (the attempt repeats next playfield
frame). Skipped attempts increment `backglass_skips`, visible in the F3
overlay page. Backglass content (scores, mode art) is produced by the game
layer on the main thread from the latest `SimSnapshot` and game ring —
the same snapshot read the playfield frame used; no extra sim access.

Perf gate (16-testing-ci.md): with both windows live on reference
hardware, playfield frame rate is unchanged (±1 fps) versus
backglass-disabled, and backglass observed rate is 30 ± 3 Hz.

## 9. Hotplug

Events: `SDL_EVENT_DISPLAY_ADDED`, `SDL_EVENT_DISPLAY_REMOVED`,
`SDL_EVENT_DISPLAY_ORIENTATION`, `SDL_EVENT_WINDOW_DISPLAY_CHANGED`.
Debounce: act 500 ms after the last such event (monitors flap during
cable/KVM changes).

```
on_display_events_settled():
    ds  = enumerate()                        # §2
    new = detect(ds, cfg)                    # skipped only if BOTH roles have
                                             # explicit matches that resolve
    for each role whose target display or rotation changed or vanished:
        mark window dirty
    if any dirty:
        SDL_WaitForGPUIdle(dev)              # 1. nothing in flight
        for each dirty window:
            SDL_ReleaseWindowFromGPUDevice(dev, win)   # 2. release swapchain
            SDL_DestroyWindow(win)                     # 3. destroy
        for each role in (playfield, backglass):       # 4. recreate, playfield first
            create + claim + set swapchain params (§7)
        renderer.on_windows_changed(...)     # 06: rebuild size-dependent targets
    write last_auto (§5) unless --display-config
    game.on_display_change()                 # 11: may pause a live game
```

The sim thread runs untouched through all of this — it owns no GPU or
window state. If the playfield display vanishes mid-game, detection
reassigns (possibly single-display fallback §10); the game layer decides
whether to pause (11-game-framework.md).

## 10. Single-display fallback

One display ⇒ playfield only (canon §5.9). Backglass content remains
reachable as a **corner overlay**, toggled with the `B` key
(05-engine-core.md §5 event table):

- The backglass scene renders into an offscreen 640×512 texture, updated
  on the same ~30 Hz cadence as §8 (render-to-texture instead of
  swapchain; still main thread, still skippable).
- Composited into the playfield frame: anchored top-right of the
  playfield viewport, width = 30 % of viewport width (height by the
  backglass canvas aspect, 13-art-direction.md), inset 8 px, opacity 85 %.
  If the §6.1 fit leaves a pillar margin ≥ 240 px (desktop landscape
  case), the overlay sits in the right pillar, bottom-aligned, width =
  margin − 16 px, fully opaque.
- Default off; toggling is runtime-only state (not persisted).
- The overlay also works in multi-display mode when
  `backglass.enabled == false` in `displays.json`.

## 11. `--windowed` dev mode

`--windowed WxH` (05-engine-core.md §2) replaces §7's fullscreen path:

- Playfield: resizable window, client size `W×H` (creation flags:
  `SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY`), on the primary
  display. Suggested default in docs: `540x960`.
- Backglass: second resizable window, 640×512, positioned 40 px to the
  right of the playfield window (clamped into the display bounds); omitted
  when `backglass.enabled == false`.
- Detection is skipped; rotation resolves to 0 unless `displays.json`
  gives an explicit value (testing rotated rendering in a window must
  work: `--windowed 960x540` + `"rotation": 90` shows the cabinet
  layout).
- Present mode: VSYNC on both windows. Frames-in-flight stays 1.
- Resize events re-run §6.1 `fit` only; no swapchain parameter changes
  (SDL handles swapchain resize internally on the next acquire).

## 12. Unit tests — detection scoring (pure, CI)

`detect()` and `auto_pf_rotation()` take plain structs and are tested in
`tests/platform/display_detect_test.cpp` with **no SDL init**. Inputs are
synthetic `DisplayInfo` lists. Required cases:

| # | Input displays (w×h@Hz, name) | Config | Expected |
|---|---|---|---|
| T1 | 1920×1080@60 "TV"; 1280×1024@60 "NEC" | none | pf=0 rot 90, bg=1 rot 0 (§4 ex. 1) |
| T2 | 1080×1920@60; 1024×768@60 | none | pf=0 rot 0, bg=1 rot 0 |
| T3 | 2560×1440@144 | none | pf=0 rot 0, bg=none |
| T4 | 1920×1080@60 "Laptop"; 2560×1440@60 "U2719" | none | pf=1 rot 0, bg=0 rot 0 |
| T5 | 1080×1920@60 | none | pf=0 rot 0, bg=none |
| T6 | 1080×1920@60; 1280×1024@60; 1920×1080@60 | none | pf=0 (portrait beats larger landscape), bg=1 (squarer than 16:9) |
| T7 | 1080×1920@60; 1440×2560@60 | none | pf=1 (larger portrait), bg=0 |
| T8 | as T1 | pf `index:1`, bg `index:0` | pf=1, bg=0 (config beats heuristics), rot: pf reported landscape? no — 1280×1024 → rot 0 auto |
| T9 | as T1 | pf `name:LG*` | warning "not found", heuristic result = T1 |
| T10 | 1920×1080@60 ×2 (identical names) | none | pf=0 (tie → lower index), bg=1, rot 0 (bg squareness 0.5625 < 0.70) |
| T11 | empty list | none | pf=none, bg=none (caller fatal) |
| T12 | as T1 | pf rotation `270` | pf=0 rot 270 (explicit passthrough), bg=1 |
| T13 | as T1 | bg `enabled:false` | pf=0, bg=none; rot 0 (no backglass ⇒ no cabinet assumption — overridable per T12) |
| T14 | as T1 | `last_auto` = {TV, NEC}, same set | identical to T1 without rescoring (stability path taken — assert via probe flag) |
| T15 | as T1 with refresh 60/75 on two 1920×1080 + NEC | none | among equal-area candidates higher Hz wins |

Every worked example in §4 must be one of these tests, byte-for-byte
identical expectations. Add a regression test whenever a real-world layout
misdetects (03-process.md).

Note T13's consequence: a single-display cabinet (rotated TV, no
backglass) auto-resolves to rotation 0 and needs `"rotation": 90` in
`displays.json`; the warning text `"landscape display without square
backglass: assuming desktop; set displays.json playfield.rotation for a
cabinet"` must be logged once at startup in that configuration.

## Common pitfalls

- **Requesting OS display rotation** (SDL orientation hints, xrandr,
  Windows display settings). Forbidden — rotation is only ever the §6
  projection matrix (canon §5.9; compositor passes cost latency).
- **Rotating by swapping viewport width/height** or scissoring instead of
  the matrix. The swapchain extent stays exactly the reported mode; only
  `P = Rz·Ortho` changes, with the *effective* viewport inside `fit()`.
- **Blocking acquire on the backglass.** One slow 60 Hz backglass present
  pins a 144 Hz playfield to 60 (ARCHITECTURE.md ADR-004 failure mode).
  Backglass uses `SDL_AcquireGPUSwapchainTexture` and skips; only the
  playfield may use `SDL_WaitAndAcquireGPUSwapchainTexture`.
- **Spawning a backglass render thread.** Canon §5.4: all GPU work on
  main. ADR-004's thread sketch is superseded.
- **Leaking the command buffer when the backglass frame is skipped.**
  Acquired command buffers must be submitted (after drawing) or cancelled
  (on skip, 06 §4.2) exactly once; leaking them exhausts the pool within
  seconds.
- **Exclusive fullscreen.** `SDL_SetWindowFullscreenMode(win, &mode)`
  blanks the other display on some platforms; always pass `NULL`
  (borderless desktop).
- **Assuming reported orientation is physical orientation.** The
  reference cabinet reports 1920×1080 for a physically-portrait TV. Never
  branch on "is portrait" without going through §3/§6.
- **Auto-rotating on desktops.** A landscape monitor without a squarish
  backglass display is a desktop; rotating gives a sideways table. Follow
  `auto_pf_rotation` exactly; the cabinet override is config.
- **Using `SDL_GetCurrentDisplayMode`.** Use `SDL_GetDesktopDisplayMode`;
  "current" can reflect a transient mode set by another app.
- **Non-deterministic tie-breaks.** All argmax comparisons are the exact
  lexicographic keys in §3; iterating an unordered container makes
  detection flap between boots and breaks T10/T14.
- **Writing `last_auto` into a `--display-config` file.** User-provided
  config paths are read-only; `last_auto` goes only to the pref-path file
  (§5), via the 05-engine-core.md §11.2 atomic write.
- **Destroying a window before releasing it from the GPU device**, or
  without `SDL_WaitForGPUIdle` first. The §9 order (idle → release →
  destroy → recreate → claim) is binding; violating it crashes in the
  driver on hotplug.
- **Re-running detection on every display event.** Debounce 500 ms (§9);
  KVMs and TVs emit event bursts.

## Done when

- [ ] `detect()` is a pure function with no SDL types in its header; all
      fifteen cases T1–T15 pass in CI on all three OSes (no display
      required).
- [ ] Reference cabinet layout (physically rotated 1920×1080 TV + 1280×1024):
      table upright on the TV, flippers at the physical bottom, scores on
      the square display — with an empty pref dir (pure heuristics).
- [ ] `displays.json` with `"rotation": 270` flips the table 180° relative
      to auto; `"match": "index:…"` and `"name:…"` overrides beat
      heuristics; a stale name falls back with the §3 warning logged.
- [ ] Desktop single-display: upright pillarboxed table, `B` toggles the
      backglass overlay in the pillar margin; overlay updates at 30 ± 3 Hz.
- [ ] Laptop + external 16:9: playfield on the larger display, unrotated;
      backglass on the other; nothing renders sideways.
- [ ] With both cabinet windows live: playfield holds the display's native
      refresh (R1 gate, 16-testing-ci.md), backglass 30 ± 3 Hz,
      `backglass_skips` visible in the F3 overlay.
- [ ] Unplugging the backglass mid-game: playfield uninterrupted (no
      dropped frames beyond the 500 ms debounce window's single
      recreation), single-display fallback active; replugging restores the
      backglass window without restart.
- [ ] `--windowed 540x960` and `--windowed 960x540` + `"rotation": 90`
      both render correctly and resize live.
- [ ] Playfield present mode is MAILBOX where supported, VSYNC otherwise,
      confirmed in the F1 overlay; frames-in-flight is 1 (single
      device-wide call site, asserted in code review).
- [ ] The §12 T13 startup warning appears in the single-landscape-display
      cabinet scenario and nowhere else.
