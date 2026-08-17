# 13 — Art Direction and the TBArt Format

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 06-rendering.md (SDF primitive capabilities, glow model, layer
draw order, fonts, segment digits, the composite pass that owns the §10 CRT
effect), 09-table-format.md (table dimensions,
light element ids, `assets/` conventions), 10-scripting.md (light control
API; owns the normative blink-pattern timing restated in §7.1).

This document is the style bible and the owner of `art.json` (the TBArt
vector format, canon §5.5). Table-authoring LLMs and humans follow it when
creating art for new tables; 14-authoring-guide.md references its worksheet.
Every rule here is checkable; tb_validate (15) enforces the ones marked
*validated*.

## 1. Style pillars

**"Neon signage at a 1962 space-age diner, filtered through a 1994 arcade."**

1. **Dark grounds.** Playfield backgrounds are near-black, tinted toward the
   palette (bg0/bg1). Light comes from neon strokes, inserts, and glow —
   never from bright fills.
2. **Saturated neon accents.** Hues are electric and pure; mid-tone pastel
   fills are forbidden on the playfield. If a shape matters, it glows.
3. **Chrome and atomic motifs.** Starbursts, orbiting electrons, boomerangs,
   chevrons, checkerboards, grid horizons. The ball is chrome; wireforms and
   rails read as tubes (`tube_outline`).
4. **Glow everywhere, but readable.** Glow marks interactive and important
   things. The ball must always be the brightest moving object; lane guides
   and shot arrows must be identifiable at a glance from 1.5 m.
5. **90s arcade energy.** Score moments burst (particles, strobe, starburst
   decals); idle moments breathe. Nothing is static during attract mode.

## 2. Palettes

### 2.1 Roles

Every table art document resolves colors through 8 named roles:

| Role | Use |
|---|---|
| bg0 | playfield ground; darkest value; the default clear color |
| bg1 | raised panels, plastics grounds, secondary fills |
| primary | the table's main neon hue: outlines, orbits, logo strokes |
| secondary | second neon hue: ramps, mode areas, contrast lines |
| accent1 | spot color: bumper caps, spinner, sparks |
| accent2 | spot color: targets, mode highlights |
| warm | amber/orange family: warnings, kickback, drain drama, flashes |
| glow_white | tinted white: specular pops, bonus lights, hottest glow cores |

### 2.2 The five canon palettes (binding hex, sRGB)

| Role | sunset-synth | atomic-teal | arcade-purple | vapor-pink | midnight-chrome |
|---|---|---|---|---|---|
| bg0 | `#0D0221` | `#06181D` | `#120521` | `#1A0B2E` | `#05070E` |
| bg1 | `#26104D` | `#0C2E36` | `#241040` | `#2E1650` | `#131B2A` |
| primary | `#FF2975` | `#1BE7D2` | `#B14EFF` | `#FF71CE` | `#4DA6FF` |
| secondary | `#08F7FE` | `#F45D48` | `#00FFC6` | `#01CDFE` | `#C4CEDF` |
| accent1 | `#9D4EFF` | `#B4E33D` | `#FF3864` | `#05FFA1` | `#00F0FF` |
| accent2 | `#FFD319` | `#FFD166` | `#3D9BFF` | `#B967FF` | `#FF4D6D` |
| warm | `#FF901F` | `#FF9F1C` | `#FF8E3C` | `#FFB347` | `#FFA940` |
| glow_white | `#FFF2E5` | `#EAFFF8` | `#F4EDFF` | `#FFF0FA` | `#EFF5FF` |

Ship-table assignments (15-launch-tables.md may not deviate): Neon Drift →
sunset-synth, Atomic Diner → atomic-teal, Tilt-O-Tron → arcade-purple,
Cosmic Carnival → vapor-pink, Voltage Vandals → midnight-chrome.
test-lab → midnight-chrome.

### 2.3 Color rules (binding)

- **Max 3 simultaneous hues per zone.** A zone (slings area, pop nest, ramp
  entrance, etc.) uses at most 3 of {primary, secondary, accent1, accent2,
  warm}; glow_white and backgrounds are free. *Validated* as a warning by
  a 0.15 m-radius sampling heuristic in tb_validate.
- **Glow color = lightened base.** When a primitive's `glow.color` is
  omitted, it defaults to `lerp(fill_color, #FFFFFF, 0.35)` computed in
  linear space (06-rendering.md §5). Explicit glow colors are for deliberate
  two-tone neon only.
- **Lane-guide contrast.** Any stroke that guides the ball (lanes, orbits,
  ramp edges) must have WCAG contrast ratio ≥ 4.5 against the ground under
  it: with linear relative luminance `Y = 0.2126R + 0.7152G + 0.0722B`,
  require `(Y_stroke + 0.05) / (Y_ground + 0.05) ≥ 4.5`. *Validated.*
- **Ball paths stay dark.** Every fill on layers z < 100 that the ball can
  roll over must have relative luminance `Y ≤ 0.40`. The chrome ball reads
  by contrast against dark ground; a bright floor erases it. *Validated:*
  tb_validate errors when a z < 100 primitive covering > 5% of the play
  area has fill `Y > 0.40`, and warns for any reachable-area fill above it.
- **warm is functional.** The `warm` role is reserved for danger/urgency
  (tilt warnings, timers expiring, drain, kickback). Do not use it for
  decoration larger than 0.02 m².

## 3. TBArt format (`art.json`)

### 3.1 Top level

```json
{
  "palette": "sunset-synth",
  "ball": { "trail": true, "trail_color": "glow_white" },
  "layers": [
    { "name": "ground",  "z": 0,   "blend": "normal",   "primitives": [ ] },
    { "name": "inserts", "z": 50,  "blend": "normal",   "primitives": [ ] },
    { "name": "wire",    "z": 140, "blend": "additive", "primitives": [ ] }
  ]
}
```

- `palette`: a canon palette name, or a custom object supplying **all 8**
  roles: `{"bg0": "#05070E", "bg1": "...", ...}`. *Validated.*
- `ball`: optional. `trail` (default false) enables the `ball_trail`
  particle effect (06-rendering.md §13); `trail_color` is a role or hex
  (default `glow_white`).
- `layers`: 1–32 entries. `z` is an integer 0–199, unique per layer.
  **z < 100 renders below the ball; 100–199 above it** (ramps, wireforms,
  upper-playfield art). `blend` is `"normal"` (premultiplied) or
  `"additive"` (pure light; use for glow-only line art). Layers render in
  ascending z. *Validated.*
- Coordinates everywhere in this file are **table meters in the physics
  coordinate system**: origin at the bottom-left of the play area as the
  player sees it (flipper end), +x right, +y up-table (canon §5.3). Art and
  physics share one space — a wall at `[0.26, 0.80]` in table.json is drawn
  by a primitive at `[0.26, 0.80]` here. Never author art in pixels or
  screen space.
- `//` comments are allowed (canon §5.5).

### 3.2 Common primitive fields

Every primitive object:

| Field | Req | Meaning |
|---|---|---|
| `kind` | yes | one of §3.3 |
| `transform` | yes | `{ "pos": [x, y], "rot_deg": 0, "scale": 1.0 }` (rot CCW, scale uniform; `rot_deg`/`scale` optional, defaults shown) |
| `fill` | varies | color, or gradient object (§3.4); shapes without area (segment, polyline) take no fill |
| `stroke` | opt | `{ "width": 0.002, "color": "primary" }` — width in meters, centered on the edge |
| `glow` | opt | `{ "radius": 0.008, "intensity": 1.0, "color": "secondary" }` — radius meters, intensity 0–2, color optional (§2.3 default) |
| `light` | opt | id of a `light` element in table.json; the primitive's fill and glow are multiplied by that light's live brightness (06-rendering.md §14.3). Unlit floor: 15% fill, 0 glow. *Validated: id must exist.* |

Colors are a role name (`"primary"`) or hex (`"#RRGGBB"` / `"#RRGGBBAA"`).

### 3.3 Primitive kinds

Shape parameters are in local meters, relative to `transform.pos`, before
`rot_deg`/`scale`.

| kind | Parameters | Notes |
|---|---|---|
| `circle` | `"r"` | filled disc |
| `ring` | `"r"`, `"thickness"` | annulus centered on r |
| `rect` | `"w"`, `"h"`, `"corner_r"` (default 0) | centered; rounded box |
| `capsule` | `"a": [x,y]`, `"b": [x,y]`, `"r"` | filled stadium |
| `segment` | `"a": [x,y]`, `"b": [x,y]` | stroke only; `stroke` required |
| `polyline` | `"points": [[x,y],...]` (2–128), `"closed"` (default false) | stroke only; lowered to capsules per segment with round joins |
| `polygon` | `"points": [[x,y],...]` (3–256, simple, CCW) | filled via ear-clipped mesh (06 §8.6); visible edges must carry `stroke ≥ 0.0015` |
| `arc` | `"r"`, `"thickness"`, `"start_deg"`, `"end_deg"` | CCW from +x, end > start, span ≤ 360 |
| `star` | `"points_n"` (4–12), `"r_outer"`, `"r_inner"` | expands to a polygon at load |
| `text` | `"string"`, `"font"` (`orbitron`\|`monoton`\|`righteous`), `"size"` (cap height, meters), `"align"` (`left`\|`center`\|`right`), `"letter_spacing"` (default 0, em fraction) | solid fill only; rendered as atlas glyphs (06 §9) |
| `decal` | `"prefab"` + `"params"` (§4), **or** `"image": "assets/name.png"` + `"w"`, `"h"` | exactly one of prefab/image. *Validated.* |

### 3.4 Fills

```json
"fill": "primary"                                             // solid role
"fill": "#FF297580"                                           // solid hex + alpha
"fill": { "type": "linear", "angle_deg": 90, "length": 0.12,
          "colors": ["primary", "accent1"] }                  // 2 stops only
"fill": { "type": "radial", "radius": 0.05,
          "colors": ["glow_white", "primary"] }
```

Gradients have **exactly 2 stops**, centered on the primitive origin
(06-rendering.md §8.4). Multi-band effects (sunsets) are built from stacked
primitives or the `sunset_semicircle` prefab. *Validated.*

### 3.5 Full example (fragment of a real table)

```json
{
  // Neon Drift, lower playfield sample
  "palette": "sunset-synth",
  "ball": { "trail": true },
  "layers": [
    { "name": "ground", "z": 0, "blend": "normal", "primitives": [
      { "kind": "rect", "transform": { "pos": [0.26, 0.52] },
        "w": 0.52, "h": 1.04,
        "fill": { "type": "linear", "angle_deg": 90, "length": 1.04,
                  "colors": ["bg0", "bg1"] } },
      { "kind": "decal", "transform": { "pos": [0.26, 0.86] },
        "prefab": "grid_horizon",
        "params": { "w": 0.44, "h": 0.26, "lines": 9, "color": "primary" } }
    ] },
    { "name": "inserts", "z": 50, "blend": "normal", "primitives": [
      { "kind": "circle", "transform": { "pos": [0.20, 0.61] }, "r": 0.011,
        "fill": "#FFB000", "glow": { "radius": 0.010, "intensity": 1.4 },
        "light": "arrow_left_ramp" },
      { "kind": "text", "transform": { "pos": [0.20, 0.585] },
        "string": "RAMP", "font": "orbitron", "size": 0.008,
        "align": "center", "fill": "glow_white" }
    ] },
    { "name": "guides", "z": 70, "blend": "normal", "primitives": [
      { "kind": "arc", "transform": { "pos": [0.26, 0.52] },
        "r": 0.245, "thickness": 0.004, "start_deg": 20, "end_deg": 160,
        "fill": "primary", "glow": { "radius": 0.008, "intensity": 1.2 } }
    ] },
    { "name": "wire", "z": 140, "blend": "additive", "primitives": [
      { "kind": "polyline", "transform": { "pos": [0, 0] },
        "points": [[0.08, 0.30], [0.10, 0.55], [0.18, 0.72]],
        "stroke": { "width": 0.003, "color": "secondary" },
        "glow": { "radius": 0.006, "intensity": 1.0 } }
    ] }
  ]
}
```

### 3.6 Recommended layer plan

Not enforced, but authoring LLMs should start here:

| z | name | contents |
|---|---|---|
| 0 | ground | full-field gradient, big motifs |
| 20 | deco | decals, checkerboards, zone panels |
| 50 | inserts | light-bound shapes + their captions |
| 70 | guides | lane/orbit strokes, shot arrows |
| 120 | over_ramps | ramp surfaces (translucent fills, alpha ≤ 0x60) |
| 140 | wire | wireform polylines, additive |
| 160 | toys | toy silhouettes, upper-playfield art |

## 4. Decal prefab catalog

`decal` prefabs expand to primitives at load; they inherit the instance
`transform` and draw in the containing layer. All `color` params accept
roles or hex; sizes are meters. Sketches are schematic, not to scale.

### 4.1 `starburst`
Params: `spikes` (6–16, default 8), `r` (default 0.03), `inner_frac` (0.35),
`color`, `glow_intensity` (1.2). Composition: a `star` polygon with stroke
0.0015 and glow; alternate long/short spikes when `spikes` ≥ 12.
```
    \ | /
   -- * --
    / | \
```

### 4.2 `atom_orbit`
Params: `r` (0.025), `orbits` (3), `electron_r` (0.003), `color`,
`nucleus_color` (default glow_white). Composition: nucleus circle r/5 +
`orbits` ellipse-approximating arcs (full rings scaled y×0.38, rotated 0°,
60°, 120°) + one electron circle per orbit at a fixed phase (0°, 140°, 250°).
```
   .--o--.
  ( (=@=) )
   `--o--'
```

### 4.3 `chevron_row`
Params: `count` (3), `w` (0.012), `h` (0.010), `gap` (0.004), `dir_deg` (90),
`color`, `glow_intensity` (1.0). Composition: `count` chevrons (2 capsules
meeting at the tip) marching along `dir_deg`; intended for shot arrows —
bind each chevron row to a light. Chevron i brightness may be chased.
```
   > > >
```

### 4.4 `checkerboard_strip`
Params: `w` (0.10), `h` (0.012), `cell` (0.006), `color_a` (bg1),
`color_b` (glow_white × alpha 0x50). Composition: alternating `rect` cells,
two rows; classic diner trim for aprons and lane borders. No glow.
```
  ▀▄▀▄▀▄▀▄
  ▄▀▄▀▄▀▄▀
```

### 4.5 `grid_horizon`
Params: `w` (0.40), `h` (0.24), `lines` (8), `color`, `fade_top` (true).
Composition: perspective floor — `lines` horizontal segments with spacing
shrinking geometrically (ratio 0.78) toward the top edge, plus 7 converging
vertical segments; stroke 0.0012, glow 0.004/0.8. Top rows use color × 0.4.
```
  \ | | | /
   \| | |/
  --+-+-+--
  ---------
```

### 4.6 `neon_arrow`
Params: `len` (0.030), `w` (0.014), `color`, `glow_intensity` (1.4),
`outline_only` (true). Composition: arrow (triangle head + rect shaft) as
polygon; outline_only draws stroke+glow with 15% fill — the standard shot
arrow, normally bound to a light.
```
    ▲
    █
```

### 4.7 `dotted_circle`
Params: `r` (0.020), `dots` (12), `dot_r` (0.0015), `color`. Composition:
`dots` circles equally spaced on the radius; marquee ring for pop bumpers
and kicker mouths. Pairs with a `chase` pattern when light-bound per-dot
groups exist; otherwise binds as one light.
```
   . ' .
  .     .
   ' . '
```

### 4.8 `boomerang`
Params: `span` (0.035), `thickness` (0.008), `color`, `rot_deg` via
transform. Composition: two capsules meeting at 100° with rounded elbow —
the googie coffee-table motif. Stroke only + glow, 15% fill.
```
   \
    \__
```

### 4.9 `lightning_bolt`
Params: `h` (0.030), `w` (0.014), `color` (default warm), `glow_intensity`
(1.6). Composition: 7-point polygon zigzag, stroke 0.0015, hot glow. Use
for kickback lanes and Voltage Vandals signage.
```
   /_
    /_
     /
```

### 4.10 `sunset_semicircle`
Params: `r` (0.045), `bands` (5), `color_top` (accent2), `color_bottom`
(primary), `gap` (0.002). Composition: `bands` stacked arcs (180°→0°) with
radii shrinking by `gap`, colors lerped top→bottom, plus 3 horizontal
`bg0`-colored slots cut across the lower half (drawn as bg0 rects on top).
The synthwave sun. Glow 0.006/1.0 on the outer band only.
```
    ___
   /███\
   ▔▔▔▔▔
   ▔▔▔
```

### 4.11 `tube_outline`
Params: `points` ([[x,y],...] 2–64), `r_tube` (0.004), `color`,
`highlight` (true). Composition: the chrome-tube look for wireforms — one
polyline stroke width 2×r_tube in color × 0.5, over-struck by a 0.35×r_tube
`glow_white` polyline offset (+0.0008, +0.0008) as the specular edge.
Intended for z ≥ 100 layers.
```
  ╔══════╗   (double-stroked rail)
```

### 4.12 `scoreburst`
Params: `r` (0.022), `color` (glow_white), `text` (optional, e.g. "1000"),
`font` (`orbitron`). Composition: `starburst(spikes=10, inner_frac=0.5)` +
centered text at size r×0.6 in bg0 (dark text on bright burst — the 90s
"score splash"). Normally light-bound and lit only during score events.
```
   \ | /
  --1000--
   / | \
```

## 5. Typography (binding)

Vendored OFL fonts in `/assets/fonts/` (license files alongside):
`Orbitron-Bold.ttf`, `Monoton-Regular.ttf`, `Righteous-Regular.ttf`.
These are **binary assets that cannot be authored or generated** — §5.1 says
exactly where they come from. Atlas baking and size selection are specified
in 06-rendering.md §14.1.

| Font | Role | Rules |
|---|---|---|
| Orbitron Bold | HUD, score labels, menus, insert captions, F1 overlay | never below 8 px rendered; letter_spacing 0–0.08 em |
| Monoton | table logos, "neon sign" display words | uppercase only (the font has no lowercase); minimum rendered height 32 px / 0.018 m — its inline strokes vanish smaller; always with glow ≥ 1.0 |
| Righteous | 60s headlines, mode names, attract taglines | title case; pair with a `boomerang` or `starburst` decal, not alone |

Playfield text sizes: insert captions 0.006–0.010 m cap height; zone
headlines 0.014–0.022 m; logo 0.030–0.050 m. Backglass (pixel space):
labels 24 px, headlines 48 px, logo/display 96 px.

Scores on the backglass use the **14-segment style** of 06-rendering.md
§14.2, not a font: default segment color = palette `primary`, ghost segments
6%, italic skew on, digit cell 64×96 px with 8 px gap, thousands separated
by the comma glyph. `Monoton` is never used for numerals.

**Meter ↔ pixel conversion** (this is what makes the px floors above
checkable). At the reference projection the playfield scale is
`ppm = min(1080 / 0.52, 1920 / 1.04) = 1846.1 px/m` — height-limited, scene
960 × 1920 (06-rendering.md §6.2, default 0.52 × 1.04 m play area):

| Authored cap height | Rendered px | Meaning |
|---|---|---|
| 0.0044 m | 8.1 px | absolute Orbitron floor (8 px); never author smaller |
| 0.006 m | 11.1 px | insert caption minimum |
| 0.010 m | 18.5 px | insert caption maximum |
| 0.014 m | 25.9 px | zone headline minimum |
| 0.018 m | 33.2 px | Monoton floor (32 px) |
| 0.022 m | 40.6 px | zone headline maximum |
| 0.030 m | 55.4 px | logo minimum |
| 0.050 m | 92.3 px | logo maximum |
| 0.0598 m | 110.4 px | **hard ceiling**: the largest baked atlas size is 96 px and upscaling beyond 1.15× is forbidden (06-rendering.md §14.1) |

A table whose play area is not 0.52 × 1.04 m recomputes `ppm` from its own
dimensions before checking the floors.

### 5.1 Font acquisition (binding; performed at M0)

Nothing in this repo can synthesize a TTF, and no milestone may stall
waiting for one. The three faces are **vendored binaries fetched once, at
M0**, and then never re-downloaded.

- **Source:** `https://github.com/google/fonts` — upstream of all three
  families, each licensed OFL 1.1.
- **Pin:** M0 resolves one commit
  (`git ls-remote https://github.com/google/fonts HEAD`) and records that
  full 40-character SHA, plus the fetch date, in `assets/fonts/SOURCES.md`.
  Every later fetch or re-verification uses that commit. Moving the pin
  afterwards requires an ADR in 02-decisions.md.
- **Files** (upstream path at the pinned commit → vendored path):

| Upstream | Vendored as |
|---|---|
| `ofl/orbitron/Orbitron-Bold.ttf`, or `ofl/orbitron/static/Orbitron-Bold.ttf` where the family has been converted to a variable font upstream | `assets/fonts/Orbitron-Bold.ttf` |
| `ofl/monoton/Monoton-Regular.ttf` | `assets/fonts/Monoton-Regular.ttf` |
| `ofl/righteous/Righteous-Regular.ttf` | `assets/fonts/Righteous-Regular.ttf` |
| `ofl/orbitron/OFL.txt`, `ofl/monoton/OFL.txt`, `ofl/righteous/OFL.txt` | `assets/fonts/Orbitron-OFL.txt`, `Monoton-OFL.txt`, `Righteous-OFL.txt` |

  Never vendor the *variable* `Orbitron[wght].ttf`: stb_truetype has no
  variation-instancing, so it would bake at the default weight (400) and the
  HUD would silently lose its Bold. Do not subset or re-save any of the
  three files — OFL's Reserved Font Name clause would then force a rename.

- **Fetch** (blobless sparse checkout; a few MB, not the whole 3 GB repo):

```sh
git clone --filter=blob:none --no-checkout https://github.com/google/fonts /tmp/gfonts
git -C /tmp/gfonts checkout <PINNED_SHA> -- ofl/orbitron ofl/monoton ofl/righteous
# equivalently, per file:
# curl -LO https://raw.githubusercontent.com/google/fonts/<PINNED_SHA>/ofl/monoton/Monoton-Regular.ttf
```

- **Integrity:** M0 runs `sha256sum` over the three vendored `.ttf` files and
  commits the output verbatim as `assets/fonts/SHA256SUMS` (`sha256sum -c`
  format, one line per file). Those hashes are the real pin — they catch an
  upstream force-push, a truncated download, or an LFS pointer committed in
  place of a font. A mismatch means the wrong bytes arrived; never
  regenerate `SHA256SUMS` to make the check pass.
- **Test:** `tests/render/font_assets_test.cpp` →
  `FontAssets.VendoredFontsPresentAndParse`, which needs no GPU and so runs
  in every CI job. For each of the three fonts it asserts: the file exists
  under `assets/fonts/`, its SHA-256 matches `SHA256SUMS`, `stbtt_InitFont`
  returns non-zero, `stbtt_GetFontVMetrics` reports ascent > 0, and
  `stbtt_FindGlyphIndex` is non-zero for every codepoint in `A`–`Z` and
  `0`–`9`. Orbitron-Bold and Righteous additionally cover all of
  U+0020–U+007E. Monoton has no lowercase — those codepoints legitimately
  fall back to `.notdef` in the §14.1 atlas, which is exactly why §5 makes
  Monoton uppercase-only. The test also asserts each `*-OFL.txt` exists and
  is non-empty (OFL 1.1 requires the license to travel with the binary).
- **Milestone:** acquisition, `SOURCES.md`, `SHA256SUMS`, the license copies,
  and that test are **M0 tasks** (04-milestones.md M0). M13 only consumes
  them; no later milestone downloads anything.
- **If the fonts cannot be obtained** (no network in the build environment,
  upstream path moved, repo unreachable): do not block and do not stub an
  empty file. 03-process.md §3.2 carries the fallback row — substitute any
  available OFL-licensed geometric/display face for the missing role, ship
  its license the same way, record the substitution as an ADR plus a
  JOURNAL.md entry, and treat the **Role/Rules columns of the §5 font
  table** as the binding requirement rather than the exact family name.

## 6. Light-insert conventions

Insert colors are **cross-table constants** so players can read any table
instantly. These override palette roles (function color beats palette
aesthetics). tb_validate warns when a light-bound primitive's function tag
(09-table-format.md light `function` field) mismatches this map.

| Function | Color | Default pattern when active |
|---|---|---|
| shot_arrow (ramps, orbits, lit shots) | amber `#FFB000` | on; `fast_blink` when the shot is hot |
| jackpot | red `#FF2B2B` | `fast_blink` |
| lock (ball lock lit / locked) | blue `#2E6BFF` | `slow_blink` lit; solid when locked |
| bonus / bonus X | glow_white role | solid |
| ball_save / shoot_again | green `#39FF6E` | `slow_blink`; `strobe` final 2 s |
| extra_ball | orange `#FF7A1A` | `fast_blink` |
| special | red `#FF2B2B` | `strobe` |
| mode / objective | palette `secondary` | `breathe` while running |
| progression (standups, top lanes) | palette `primary` | solid when made; `slow_blink` when needed |
| multiball_ready | palette `accent2` | `fast_blink` |
| status / GI accents | palette `primary` × 0.5 | solid or `breathe` |

Unlit inserts always remain visible at 15% fill (06-rendering.md §14.3) so
the playfield reads as a map even when dark.

## 7. Animation guidelines

### 7.1 Named blink patterns (timing normative in 10-scripting.md §3.2)

`tb.light_blink(id, pattern)` accepts exactly these names; the scripting
layer resolves them to the numeric rate/duty/mode the renderer consumes
(06-rendering.md §14.3). Timing is owned by 10-scripting.md §3.2 — the
Rate/Duty values below restate it; only the Use column is this document's:

| Name | Mode | Rate | Duty | Use |
|---|---|---|---|---|
| `slow_blink` | square | 2 Hz | 50% | "come here" — lit shots, ball save |
| `fast_blink` | square | 5 Hz | 50% | urgent — jackpot, hurry-up |
| `strobe` | square | 10 Hz | 20% | climax moments only, ≤ 3 s bursts |
| `chase` | chase | advance every 80 ms per lamp, group declaration order | — | arrows/dot rings pointing along a path |
| `breathe` | sine | 0.625 Hz | 15–100% depth | idle/mode ambience, GI |

### 7.2 Attract mode choreography (framework/art-driven — no Lua)

**Attract is framework/art-driven. There is no Lua attract hook and no
attract-time `lua_State` in v1.** While the game is in Attract no table
`rules.lua` is loaded and no script runs (11-game-framework.md §8.2 owns the
state and releases the loaded sim); the show below is a **fixed framework
routine** played by the game/render layer on wall-clock time, parameterized
only by data the table already declares: each `light` element's `function`
tag (§6, `table.json` per 09-table-format.md), its playfield position, and
the table's `light_groups`. A table personalizes its attract look through
`art.json` and those tags — never through script.

Because no sim ticks in Attract, this animation is wall-clock, outside the
replay record; it cannot affect determinism.

The loop is 15 s, restarted for as long as no game is active:

1. **0–8 s:** every light tagged `status` or `progression` runs `breathe`,
   phase-offset bottom-to-top so the wave climbs the table: phase =
   `0.4 s × min(4, floor(y / (table_h / 5)))`, i.e. five bands of 0.208 m
   across the default 1.04 m field ⇒ offsets 0.0 / 0.4 / 0.8 / 1.2 / 1.6 s
   (the `min` keeps a light sitting exactly on the top edge in band 4).
2. **8–12 s:** `chase` on the largest declared `light_groups` entry (ties →
   first in declaration order), advancing in declaration order at the §7.1
   80 ms/lamp rate — that group is the table's longest guide path in
   practice. Meanwhile all `shot_arrow` lights `slow_blink`. A table that
   declares no light group holds step 1 through this window.
3. **12–13 s:** every `jackpot`, `special`, and `multiball_ready` light
   `strobe`s, plus one `jackpot_starburst` effect at the play-area center
   (`table_w/2, table_h/2` = 0.26, 0.52 m on the default field).
4. **13–15 s:** everything off except the §6 unlit-ghost floor (dark beat),
   then loop.

`tb_screenshot --views attract` samples this loop at **t = 4.0 s, 10.0 s and
12.5 s** (one frame per step 1/2/3) into `attract_0.png`–`attract_2.png`, so
those golden images are reproducible (06-rendering.md §15.2).

### 7.3 Mode-start flourish (guidance)

On mode start: the mode insert `strobe` for 0.5 s then `breathe`; one
starburst effect at the mode's shot; related shot arrows `chase` for 2 s
then steady; music sting (12-audio.md). Keep total flourish under 1 s of
strobe — longer reads as a fault, not excitement.

## 8. Particle style rules

Effects and binding parameters live in 06-rendering.md §13.4. Style rules
for any new per-table effect an author requests via `EffectRequest`:

- Colors: palette accents and `glow_white` only; never bg roles.
- Blend: additive always; premultiplied spawn brightness ≤ 1.4.
- Lifetimes: bursts 0.2–1.0 s; trails 0.15–0.6 s; nothing above 1.2 s.
- Sizes: 0.5–6 mm; particles are garnish, never occluders.
- Density: one gameplay event ⇒ ≤ 100 particles; celebration (jackpot,
  multiball) ⇒ ≤ 300 across all emitters.

## 9. Per-table theming worksheet

14-authoring-guide.md requires this filled worksheet before any `art.json`
is written. Copy verbatim:

```markdown
## Theming worksheet — <table name>
- Mood words (exactly 3): <e.g. "midnight, chrome, velocity">
- Palette: <canon palette name, or custom + 8 hex values and why>
- Signature motifs (exactly 3 prefabs/shapes and where they repeat):
  1. <e.g. grid_horizon on the ground, echoed small behind the pops>
  2. <...>
  3. <...>
- Logo text + font: <"NEON DRIFT", monoton>
- Layer plan: <list of layers with z, one line each, per 13 §3.6>
- warm-role moments: <which events use warm: tilt warning, ...>
- Light-function inventory: <each insert: function tag from 13 §6>
```

## 10. Optional CRT effect (off by default)

Config `render.crt = false` (the key is declared in 05-engine-core.md §11.1).
When enabled it is applied inside the composite pass, after bloom, before
sRGB encode.

**06-rendering.md §12.5 is the implementation owner** — the shader branch, its
uniform, the pixel space, and the cost budget live there, and 04-milestones.md
M13 carries it in scope. This section owns the *look*; §12.5 restates these
formulas verbatim, so a change here is a change there:

- Scanlines: multiply luminance by `1 - 0.12 * square_wave(py, period 3 px)`
  (dark line 1 px of every 3; `py` is a scene/logical-pixel row, so the lines
  run across the image as the player sees it even on a rotated panel).
- Vignette: multiply by `1 - 0.15 * smoothstep(0.6, 1.0, r_norm)` from
  screen center (`r_norm` = 0 at the image center, 1 at a corner, elliptical
  in the image, so edge midpoints darken only ≈ 2.7 %).
- No barrel distortion, no chromatic aberration, no phosphor mask — they
  blur the ball.

CRT mode must never be enabled by a table; it is a user setting only. Golden
images are unaffected either way: `tb_screenshot` always composites with CRT
off (06-rendering.md §12.5), as it already does for particles.

## 11. Dos and don'ts

- **Do** put a gradient on the ground (`bg0`→`bg1`, angle 90°) — flat black
  looks unfinished. **Don't** exceed `bg1` luminance on the ground.
- **Do** give every interactive target a glow ≥ 0.8 intensity. **Don't**
  glow pure decoration above 1.0 — save headroom for gameplay.
- **Do** outline shots with `arc`/`polyline` strokes 0.003–0.005 m wide.
  **Don't** draw guide strokes thinner than 0.002 m (invisible at 1.5 m).
- **Do** repeat one motif at three scales (worksheet). **Don't** use more
  than 4 prefab types per table — collage kills identity.
- **Do** use `additive` layers for wireforms and pure-light art. **Don't**
  put text or inserts on additive layers (unreadable over bright areas).
- **Do** keep translucent ramp fills at alpha ≤ 0x60 so the ball reads
  through (z 120). **Don't** fully occlude any ball path with z ≥ 100 art
  wider than 0.03 m without a `tube_outline` gap showing the ball.
- **Do** author in meters with the physics origin. **Don't** mirror or
  offset art to "look right" — if art and colliders disagree, the table is
  wrong, not the camera.
- Example of a correct hot shot arrow: `neon_arrow` at the ramp mouth,
  `#FFB000`, glow 0.010/1.4, light-bound, `fast_blink` when lit — never a
  static bright triangle.

## 12. Accessibility: flash reduction

Setting `accessibility.reduce_flashing = true` (menus, 11-game-framework.md
§10; settings key per 05-engine-core.md; default false). Applied at render/spawn time only — the sim and scripts are
untouched, so determinism and replays are unaffected (06-rendering.md
§13.4, §14.3):

- Any light pattern rate > 3 Hz is clamped to 3 Hz and duty raised to
  ≥ 50% (`strobe` and `fast_blink` become 3 Hz half-duty).
- Fullscreen flash overlay intensity × 0.5.
- Particle spawn brightness × 0.6; `jackpot_starburst` count halved.
- Attract choreography step 3 (§7.2) renders `strobe` as `slow_blink`.

This mode must remain playable-first: no gameplay information may exist
*only* in a > 3 Hz flash. *Validated:* tb_validate warns if a table's rules
use `strobe` as the sole indicator of a timed event (heuristic: strobe with
no paired sound cue registered in audio.json).

## 13. Style checklist

04-milestones.md M13 gates its beauty pass on "the 13-art-direction.md style
checklist" — **this is that checklist**. Every item is decidable from
`tb_screenshot <table> --views full,lower,upper,backglass,attract` output
plus the table's `art.json`; nothing here needs taste. Items marked
*(V0xx)* are additionally machine-checked by tb_validate (09-table-format.md
§ validation rules), so a failure there is a build problem, not an opinion.
Paste this list into the M13 (and every later art) PR body with each box
ticked.

**Color and contrast (§2.3)**

- [ ] No fill on a layer `z < 100` that the ball can roll over exceeds
      relative luminance `Y = 0.40`; the chrome ball is the brightest moving
      object in every frame. *(V036)*
- [ ] Every lane / orbit / ramp-edge guide stroke reaches contrast ≥ 4.5:1
      against the ground under it,
      `(Y_stroke + 0.05) / (Y_ground + 0.05) ≥ 4.5`. *(V036)*
- [ ] At most 3 of {primary, secondary, accent1, accent2, warm} appear in
      any 0.15 m-radius sample; glow_white and bg roles are free.
- [ ] `warm` appears only on danger/urgency art, never as decoration larger
      than 0.02 m².
- [ ] Glow budget: emissive area in the default (non-mode) light state
      ≤ 15 % of the play area — **0.081 m²** of the default 0.52 × 1.04 m
      field (0.5408 m²). *(V035)*

**Typography (§5)**

- [ ] No playfield text below the 8 px floor — at the reference
      `ppm = 1846.1 px/m` that is 0.0044 m cap height. Insert captions
      0.006–0.010 m, zone headlines 0.014–0.022 m, logo 0.030–0.050 m.
- [ ] Monoton is uppercase-only, never below 0.018 m / 32 px, always with
      glow ≥ 1.0.
- [ ] No text above 0.0598 m / 110 px (96 px largest baked size × 1.15 max
      upscale, 06-rendering.md §14.1).
- [ ] Backglass: labels 24 px, headlines 48 px, logo 96 px; scores are
      14-segment digits in `primary` with 6 % ghosts and comma separators —
      no font, and never Monoton, for numerals.
- [ ] The three fonts on screen are the §5.1 vendored files (or a
      documented ADR substitution), not a system font.

**Inserts and light (§6, §7)**

- [ ] Every light-bound primitive's color matches its `function` tag in the
      §6 map — amber shot_arrow, red jackpot/special, blue lock, green
      ball_save, orange extra_ball — even where the palette would prefer
      otherwise. *(V031)*
- [ ] Unlit inserts remain readable at the 15 % ghost floor: the dark
      playfield still reads as a map.
- [ ] Only the five §7.1 pattern names are used, and no `strobe` runs
      longer than 3 s.
- [ ] The three `attract_*.png` frames show §7.2 steps 1–3 (breathe wave,
      group chase, strobe burst) driven by light `function` tags alone — no
      table script is involved — and no frame is a wall of bloom.

**Composition (§11 don'ts)**

- [ ] Ground carries a bg0→bg1 gradient (not flat black) and never exceeds
      bg1 luminance.
- [ ] Guide strokes are 0.003–0.005 m wide (never < 0.002 m); every
      interactive target glows ≥ 0.8; no purely decorative glow above 1.0.
- [ ] At most 4 decal prefab types on the table, with one motif repeated at
      three scales (§9 worksheet).
- [ ] Additive layers carry only wireform / pure-light art — no text, no
      inserts.
- [ ] Ramp fills at z 120 are alpha ≤ 0x60, and no `z ≥ 100` art wider than
      0.03 m hides a ball path without a `tube_outline` gap.
- [ ] Art and colliders coincide: the F2 overlay screenshot shows ≤ 1 px
      deviation — nothing mirrored, offset, or authored in pixel space.

## Common pitfalls

- **Authoring art in screen/pixel coordinates or top-left origin.** Art
  lands mirrored or off-table. Correct: table meters, origin bottom-left at
  the flipper end, +y up-table — identical to table.json (§3.1).
- **Bright playfield floors.** A cream or pastel ground makes the chrome
  ball invisible and fails validation. Correct: ground fills from bg0/bg1
  only; ball-path luminance ≤ 0.40 (§2.3).
- **Palette-colored function inserts.** Painting jackpot in the table's
  primary hue breaks cross-table readability. Correct: function colors from
  §6 always win over palette aesthetics.
- **Multi-stop gradients or per-stop positions.** TBArt fills have exactly
  2 stops (§3.4); banded skies must use stacked primitives or
  `sunset_semicircle`. The loader must reject, not silently truncate.
- **Putting ramp/wire art below z 100.** The ball then rolls "over" art
  that should cover it. Correct: anything physically above the ball's plane
  goes in z 100–199 (§3.1).
- **Monoton at small sizes or lowercase.** Its inline strokes collapse and
  it has no lowercase (those codepoints resolve to `.notdef`). Correct:
  uppercase, ≥ 32 px / 0.018 m, always with glow (§5).
- **Treating the fonts as authorable, or re-fetching them mid-project.** No
  LLM can emit a valid TTF, and a milestone that tries to download one
  stalls. Correct: the three files arrive **once at M0** from the §5.1
  pinned commit, each with its `OFL.txt` and a committed SHA-256; if the
  fetch is impossible, take the 03-process.md §3.2 substitution fallback and
  record an ADR — never commit a placeholder, a zero-byte file, or the
  variable `Orbitron[wght].ttf`.
- **Choreographing attract in `rules.lua`.** There is no Lua attract hook
  and no attract-time `lua_State` in v1, so such a script simply never runs
  and the table looks dead in the cabinet's most-seen state. Correct:
  attract is a fixed framework routine driven by light `function` tags,
  `light_groups`, and `art.json` (§7.2).
- **Decorating with `warm`.** Amber/orange is a semantic channel for danger
  and urgency; decorative use trains players to ignore warnings (§2.3).
- **Unbounded strobe.** Strobe longer than 3 s or as ambience is both
  unpleasant and inaccessible. Correct: ≤ 3 s bursts at climaxes; flash
  reduction must have something left to reduce (§7.1, §12).
- **Forgetting the unlit-ghost floor.** Inserts that vanish when off make
  the table unlearnable. Correct: 15% fill floor is automatic — do not
  fight it by binding a black fill to a light (§6).
- **Inventing primitive kinds or prefab params.** The schema is closed;
  tb_validate rejects unknown fields. Correct: compose §3.3 kinds and §4
  prefabs only, or extend this spec by ADR first.

## Done when

- [ ] All five canon palettes with the exact §2.2 hex values are compiled
      into `tb_table` and selectable by name; a custom-palette art.json with
      all 8 roles loads; one with 7 roles is rejected.
- [ ] The full §3 TBArt schema loads: every primitive kind, both fill
      gradient types, stroke, glow, transform, `light` binding, `ball.trail`,
      normal and additive layers — proven by a dedicated loader fixture
      (`tests/fixtures/art_all_kinds.json`) that uses every kind at least
      once. test-lab's `art.json` is deliberately minimal (09-table-format.md
      §7.1 pins it byte-for-byte) and is *not* the coverage vehicle.
- [ ] z < 100 art renders below the ball and z ≥ 100 above it in a
      screenshot with the ball mid-table.
- [ ] Art coordinates align with physics: an F2 overlay screenshot of a
      shipped table (whose art strokes its collider outlines) shows ≤ 1 px
      deviation between art and collider geometry.
- [ ] All 12 decal prefabs expand and render per their §4 compositions;
      `tb_screenshot --art-only` of the prefab-sheet fixture table
      (`tests/fixtures/prefab_sheet/`, one instance of each decal on a plain
      ground) matches its golden image.
- [ ] The three OFL fonts are vendored **at M0** per §5.1 — fetched from the
      pinned `github.com/google/fonts` commit recorded in
      `assets/fonts/SOURCES.md`, each with its `*-OFL.txt` and a line in
      `assets/fonts/SHA256SUMS` — and
      `FontAssets.VendoredFontsPresentAndParse` is green in every CI job
      (or an ADR records the 03-process.md §3.2 substitution). They render
      per the §5 rules, including the meter↔pixel floors; backglass scores
      render in the 14-segment style with primary color, ghosts, and comma
      separators.
- [ ] The §6 function-to-color map is applied by all five shipped tables;
      tb_validate emits the mismatch warning on a deliberately wrong fixture.
- [ ] The five §7.1 pattern names resolve through `tb.light_blink` to the
      exact 10-scripting.md §3.2 rates/duties/modes (restated in §7.1),
      verified by a replay-locked light sequence test.
- [ ] tb_validate implements every rule marked *validated* (palette
      completeness, layer z, lane contrast, ball-path luminance, light-id
      existence, 2-stop fills, decal exclusivity, polygon validity) with
      fixture tables that fail each rule.
- [ ] Flash-reduction mode demonstrably clamps strobe to 3 Hz, halves the
      flash overlay, and halves jackpot_starburst count, with the sim replay
      unchanged.
- [ ] CRT mode off by default (`render.crt`, 05-engine-core.md §11.1); when
      enabled, the 06-rendering.md §12.5 composite branch produces the §10
      scanlines and vignette (one scene row in three at 0.88×, corners
      0.85×), no extra pass appears, and the setting survives restart.
- [ ] The §9 worksheet exists verbatim in this file and is referenced by
      14-authoring-guide.md's authoring sequence.
- [ ] The §13 style checklist is ticked item-by-item in the M13 PR body
      against `tb_screenshot` output, and each *(V0xx)*-marked item is
      actually enforced by the named validator rule (V031, V035, V036)
      rather than by eyeballing.
- [ ] Attract plays its §7.2 choreography with **no table `lua_State`
      alive** — proven by entering Attract on a table whose `rules.lua`
      would throw on load: the light show, wave phases, chase, and starburst
      still run, and the three `attract_*.png` frames at t = 4.0 / 10.0 /
      12.5 s match their goldens.
