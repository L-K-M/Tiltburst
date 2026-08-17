# 06 — Rendering

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 02-decisions.md (ADR-005 renderer interface, ADR-012 prebuilt
shader blobs), 05-engine-core.md (main loop, timing ring, config,
SimSnapshot/SimEvent plumbing), 07-displays.md (windows, rotation config),
08-physics.md (snapshot ball/element state), 09-table-format.md (table
dimensions, layers), 13-art-direction.md (TBArt document, palettes, blink
pattern names).

This document specifies the entire `tb_render` library and the `/shaders`
tree, to the level where no visual decision is left to the implementor.

## 1. Scope and non-goals

The renderer is a 2D, instanced, SDF-first forward renderer on the SDL3 GPU
API. It renders the playfield window at native refresh (never below 60 fps,
canon §1), the backglass window at ~30 Hz without ever stalling the
playfield, an HDR intermediate with a bloom post chain, plus particles,
text, light inserts, debug overlays, and screenshots.

Non-goals for v1: 3D meshes, shadows, MSAA (SDF edges are analytically
antialiased), dynamic cubemaps (the ball uses a procedural fake-chrome
gradient), HDR output surfaces, TAA (forbidden — ARCHITECTURE.md §3).

## 2. Renderer boundary (ADR-005)

ADR-005 keeps the door open for a raw Vulkan backend:

- `src/render/renderer.h` is the **only** render header included outside
  `tb_render`, and it must not include or mention any `SDL_GPU*` type.
  `SDL_Window*` is permitted (a Vulkan backend consumes it too).
- GPU concepts cross the boundary as POD structs and opaque 32-bit
  generational handles. The game/table layers build a `RenderFrame`
  description; the renderer consumes it. No callbacks into game code.

```cpp
// src/render/renderer.h   (namespace tb::render)
struct TextureHandle { uint32_t id = 0; };   // 0 = invalid
struct RendererConfig {
  SDL_Window* playfield_window;      // required
  SDL_Window* backglass_window;      // nullptr => single-display mode
  Rotation    playfield_rotation;    // ROT_0/90/180/270, from 07-displays.md
  bool        debug_device;          // SDL GPU debug mode
  bool        prefer_mailbox;        // canon §5.4
};
struct RenderStats;                  // §17
class IRenderer {
public:
  virtual ~IRenderer() = default;
  virtual bool init(const RendererConfig&) = 0;
  virtual void shutdown() = 0;
  virtual bool load_table(const TableRenderData&) = 0;   // §8.6, §9
  virtual void unload_table() = 0;
  // Per frame, main thread only (canon §5.4):
  virtual void render_playfield(const RenderFrame&) = 0;
  virtual bool render_backglass(const BackglassFrame&) = 0; // false = skipped
  virtual void request_screenshot(const char* png_path) = 0; // §15
  virtual const RenderStats& stats() const = 0;
};
std::unique_ptr<IRenderer> make_sdl_gpu_renderer();      // only factory in v1
```

`RenderFrame` carries: pointer to the latest `SimSnapshot` (never
interpolated, §14.4), frame wall-dt for particles, HUD text runs, debug
toggles, and the flash-overlay state (§13.5). `TableRenderData` carries the
parsed TBArt document (13-art-direction.md), ear-clipped polygon meshes,
decoded PNG decals, and the light-binding table. Replacing the backend means
reimplementing `IRenderer` and nothing else.

## 3. Device and swapchain initialization

Startup order (main thread):

```c
SDL_GPUDevice* dev = SDL_CreateGPUDevice(
    available_shader_formats(),      // §16.4: on-disk formats ∩ SPIRV|DXIL|MSL
    /*debug_mode=*/ cli.dev || TB_DEV_BUILD,   // exactly as 05 §1 step 9
                                     // TB_DEV_BUILD: true in Debug/RelWithDebInfo,
                                     // false in shipping Release (CMake option
                                     // TB_DEV). `--dev` also forces the debug
                                     // layer on in a Release build (05 §2).
    /*name=*/ NULL);
if (!dev) fail("SDL_CreateGPUDevice: %s", SDL_GetError());

SDL_ClaimWindowForGPUDevice(dev, playfield_window);      // playfield FIRST
if (backglass_window) SDL_ClaimWindowForGPUDevice(dev, backglass_window);

bool mailbox = SDL_WindowSupportsGPUPresentMode(dev, playfield_window,
                                                SDL_GPU_PRESENTMODE_MAILBOX);
SDL_SetGPUSwapchainParameters(dev, playfield_window,
    SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
    mailbox ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC);
if (backglass_window)
  SDL_SetGPUSwapchainParameters(dev, backglass_window,
      SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

SDL_SetGPUAllowedFramesInFlight(dev, 1);                 // canon §5.4
```

Rules:

- Frames-in-flight is **1, device-wide**. ARCHITECTURE.md §7's table
  (backglass thread, FIF 2) is superseded by canon §5.4: one device, one
  thread, FIF 1; the backglass cannot stall anything because its swapchain
  acquire is non-blocking (§4.2).
- Never exclusive fullscreen (ARCHITECTURE.md §3 rule 4); windows come from
  07-displays.md as borderless fullscreen.
- Query each window's swapchain format via
  `SDL_GetGPUSwapchainTextureFormat` after claiming; swapchain-target
  pipelines are created against it (§7).
- If device creation fails, `tiltburst` exits with a clear error; no
  software fallback (tools: §15.3).

## 4. Frame lifecycle (main thread)

### 4.1 Playfield frame

```
render_playfield(frame):
  poll_gpu_fence()                                   # §17.2 GPU-time counter
  cmd = SDL_AcquireGPUCommandBuffer(dev)
  if !SDL_WaitAndAcquireGPUSwapchainTexture(cmd, playfield_win,
                                            &sc_tex, &sc_w, &sc_h):
      SDL_CancelGPUCommandBuffer(cmd); return        # minimized/occluded: skip
  if letterbox_changed(sc_w, sc_h): recreate_scene_targets()   # §6.3
  build_dynamic_instances(frame)     # CPU: lights, ball, particles, text, debug
  copy pass: upload dynamic instance buffers         # cycle=true on all buffers
  pass A: scene       -> scene_color (RGBA16F)       # §7
  passes B..E: post   -> bloom chain                 # §12
  pass F: composite   -> sc_tex (rotation+letterbox) # §12.5
  pass G: debug + overlay text -> sc_tex             # §16, only if F1/F2 active
  if screenshot_pending: extra draw + copy -> readback  # §15.1
  gpu_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd)
```

### 4.2 Backglass frame (non-blocking, ~30 Hz)

Runs right after the playfield submit, only when
`now - last_backglass_present >= 33 ms`:

```
render_backglass(frame):
  bg = SDL_AcquireGPUCommandBuffer(dev)
  if !SDL_AcquireGPUSwapchainTexture(bg, backglass_win, &tex, &w, &h)  # NON-blocking
     || tex == NULL:
      SDL_CancelGPUCommandBuffer(bg); return false   # not ready -> skip frame
  pass: clear to palette bg0, draw backglass DrawList (pixel space, §6.4)
  SDL_SubmitGPUCommandBuffer(bg)
  last_backglass_present = now; return true
```

A NULL texture from the non-wait acquire is the designed skip path, not an
error. The backglass gets no bloom in v1 (glow via SDF glow terms only).
Never call any wait function on the backglass path.

### 4.3 Threading

Every function in this document runs on the **main thread** (canon §5.4).
`tb_render` contains zero thread primitives except the atomic read of the
snapshot pointer published by the sim thread (05-engine-core.md).

## 5. Color management

- All authored colors (palettes, art.json, particle tables) are **sRGB** hex.
- At load they are converted sRGB→linear per channel and alpha-premultiplied.
  The whole pipeline — fills, glow, particles, bloom — operates on
  **premultiplied linear** RGBA.
- `scene_color` and all bloom targets: `SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT`.
- The composite pass (§12.5) applies the exact piecewise sRGB encode, then
  writes to the UNORM swapchain.
- Pass G and the backglass draw directly in sRGB space: colors authored in
  sRGB, written without conversion (fragment variants compiled with
  `TB_ENCODE_SRGB=0`, §16.4).

## 6. Coordinate transforms

### 6.1 Spaces

| Space | Units | Origin | +y | Used by |
|---|---|---|---|---|
| Table | meters | bottom-left of play area | up-table | art, physics, particles, lights, ball (canon §5.3) |
| Scene px | pixels | bottom-left | up | scene render target (§6.3) |
| Swapchain NDC | clip | center | up (SDL GPU convention) | composite |
| Backglass px / UI px | pixels | **top-left** | **down** | backglass UI, F1 overlay |

SDL3 GPU normalizes all backends to D3D-style conventions: NDC +x right,
+y **up**, z in [0,1]; texture coordinates top-left origin. Never add
per-backend flips.

### 6.2 Rotation and letterbox

Physical swapchain is `(sc_w, sc_h)`. Rotation `rot ∈ {0,90,180,270}` comes
from 07-displays.md (use its enum and sign convention verbatim).

```
(lw, lh) = (rot == 90 || rot == 270) ? (sc_h, sc_w) : (sc_w, sc_h)  # logical px
ppm      = min(lw / table_w, lh / table_h)          # pixels per meter, float
scene_w  = round(table_w * ppm); scene_h = round(table_h * ppm)
```

Reference cabinet (`1920×1080` reported landscape, rot 90, table
0.52 × 1.04 m): `lw=1080, lh=1920, ppm=1846.1`, scene = 960 × 1920,
60 px bars each side.

```
                 swapchain 1920x1080 (landscape panel, rotated 90)
   +--------------------------------------------------+
   |####|                                        |####|      # = black bars
   |####|   scene target 960x1920 drawn rotated  |####|
   |####|   table (0,0) at the player/flipper end|####|
   +--------------------------------------------------+
```

### 6.3 Table → scene target

`scene_color` is allocated at exactly `scene_w × scene_h`, **portrait,
unrotated**; every pass except F and G works in this orientation. The
table→clip transform for the scene pass is a pure scale (the target covers
exactly the play area):

```
x_ndc = p.x / table_w * 2 - 1
y_ndc = p.y / table_h * 2 - 1          # table +y (up-table) == NDC +y
```

Recreate scene + bloom targets whenever `(scene_w, scene_h)` changes.

### 6.4 Composite rotation, and pixel spaces

Pass F draws one quad covering the letterboxed rect: compute the rect's
corners in **logical** NDC, then rotate each into swapchain NDC:

| rot | (x, y) → |
|---|---|
| 0   | ( x,  y) |
| 90  | (-y,  x) |
| 180 | (-x, -y) |
| 270 | ( y, -x) |

The swapchain is cleared to opaque black first (the bars). Table-space debug
draw (§16) reuses the same composed transform. UI pixel space (backglass,
F1) maps top-left-origin pixels to NDC as
`x_ndc = 2*px/w - 1; y_ndc = 1 - 2*py/h` — note the y flip.

### 6.5 View uniforms

One block, pushed per pass with `SDL_PushGPUVertexUniformData(cmd, 0, ...)`
(vertex `register(b0, space1)`; fragment copy at `register(b0, space3)`):

```hlsl
cbuffer ViewUniforms : register(b0, space1) {
  float4x4 u_to_clip;    // space -> clip for this pass
  float2   u_target_px;  // current render target size
  float    u_ppm;        // pixels per meter (0 in pixel-space passes)
  float    u_time;       // seconds = snapshot.tick * 0.001 (NEVER wall clock)
};
```

## 7. Render pass graph and pipeline inventory

Targets (linear-clamp sampler unless noted):

| Target | Format | Size | Notes |
|---|---|---|---|
| scene_color | RGBA16F | scene_w × scene_h | HDR scene |
| bloom0 / blur0 | RGBA16F | 1/2 res | ping-pong pair |
| bloom1 / blur1 | RGBA16F | 1/4 res | ping-pong pair |
| bloom2 / blur2 | RGBA16F | 1/8 res | ping-pong pair |
| readback | RGBA8_UNORM | swapchain size | only while screenshot pending |
| font_atlas | R8_UNORM | 2048 × 2048 | §14.1, static |
| decal textures | RGBA8_UNORM | ≤1024² each | premultiplied+linearized, mipped |

Graphics pipelines (created once at init; `*_srgb` fragment variants are the
same HLSL compiled with `-DTB_ENCODE_SRGB=0` for direct-to-swapchain passes):

| # | Pipeline | VS / FS | Blend | Target fmt |
|---|---|---|---|---|
| 1 | sdf_premul | sdf.vert / sdf.frag | premultiplied | RGBA16F |
| 2 | sdf_additive | sdf.vert / sdf.frag | additive | RGBA16F |
| 3 | mesh_premul | mesh.vert / mesh.frag | premultiplied | RGBA16F |
| 4 | mesh_additive | mesh.vert / mesh.frag | additive | RGBA16F |
| 5 | sprite_premul | sprite.vert / sprite.frag | premultiplied | RGBA16F |
| 6 | sprite_additive | sprite.vert / sprite.frag | additive | RGBA16F |
| 7 | particle | particle.vert / particle.frag | additive | RGBA16F |
| 8 | brightpass | fs_tri.vert / brightpass.frag | disabled | RGBA16F |
| 9 | downsample | fs_tri.vert / downsample.frag | disabled | RGBA16F |
| 10 | blur | fs_tri.vert / blur.frag | disabled | RGBA16F |
| 11 | upsample_add | fs_tri.vert / upsample.frag | additive | RGBA16F |
| 12 | composite | composite.vert / composite.frag | disabled | swapchain |
| 13 | sdf_ui | sdf.vert / sdf.frag (srgb) | premultiplied | swapchain |
| 14 | sprite_ui | sprite.vert / sprite.frag (srgb) | premultiplied | swapchain |

Blend states, exactly:

```
premultiplied: color: srcFactor=ONE dstFactor=ONE_MINUS_SRC_ALPHA op=ADD
               alpha: srcFactor=ONE dstFactor=ONE_MINUS_SRC_ALPHA op=ADD
additive:      color: srcFactor=ONE dstFactor=ONE               op=ADD
               alpha: srcFactor=ZERO dstFactor=ONE              op=ADD
```

No depth buffer anywhere. Painter's order: TBArt layers by `z` ascending,
authored order within a layer; ball(s) draw between z 99 and z 100
(13-art-direction.md owns the z convention).

## 8. Pipeline 1: SDF primitives

The workhorse. One draw call per (layer, blend) run of instances.

### 8.1 Geometry and instancing

Vertex buffer slot 0: static unit quad, `float2 corner` in {-1,+1}²,
4-vertex triangle strip, `num_instances = N`. Slot 1: per-instance data with
`SDL_GPU_VERTEXINPUTRATE_INSTANCE`.

```c
struct SdfInstance {                  // 128 bytes, 16-byte aligned rows
  float cx, cy;                       // center, table meters (or UI px)
  float hx, hy;                       // quad half-extents INCLUDING glow pad
  float rot;                          // radians CCW
  uint32_t kind;                      // PrimitiveKind, below
  float p0, p1;                       // shape params (per-kind table)
  float p2, p3, p4;                   // shape params
  float glow_radius;                  // meters; 0 = no glow
  float fill0[4];                     // premult linear RGBA, gradient stop 0
  float fill1[4];                     // premult linear RGBA, gradient stop 1
  float grad[4];                      // xy=unit dir, z=length|radius, w=mode
                                      //   mode 0=solid 1=linear 2=radial
  float stroke[4];                    // rgb premult linear, w = width (m); 0=none
  float glow[4];                      // rgb linear, w = intensity [0,2]
};
```

| kind | value | p0..p4 (local meters, pre-rotation, relative to center) |
|---|---|---|
| CIRCLE | 0 | r |
| RING | 1 | r, half_thickness |
| RBOX | 2 | half_w, half_h, corner_r |
| CAPSULE | 3 | ax, ay, bx, by, r |
| ARC | 4 | r, half_thickness, a0_rad, a1_rad (CCW from +x, a1 > a0) |
| BALL | 5 | r  (§11 shading path) |

TBArt `segment`/`polyline` lower to CAPSULE instances at load; `star` lowers
to a polygon (13-art-direction.md). Quad half-extents:
`h = shape_bounds_half + stroke.w * 0.5 + 1.25 * glow_radius` — the 1.25 pad
keeps the exponential glow tail from clipping (`exp(-5) ≈ 0.007`).

### 8.2 Vertex shader (sdf.vert)

```hlsl
VSOut main(float2 corner : TEXCOORD0, SdfInstance inst) {
  float2 lp = corner * inst.h;                        // local, meters
  float2 wp = inst.c + rot2(inst.rot, lp);            // table space
  o.pos = mul(u_to_clip, float4(wp, 0, 1));
  o.lp = lp;  o.inst_id = instance_id;                // rest fetched in FS
}
```

### 8.3 Fragment shader (sdf.frag) — SDF functions

```hlsl
float sd_circle (float2 p, float r)                  { return length(p) - r; }
float sd_ring   (float2 p, float r, float ht)        { return abs(length(p) - r) - ht; }
float sd_rbox   (float2 p, float2 b, float cr) {
  float2 q = abs(p) - b + cr;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - cr;
}
float sd_capsule(float2 p, float2 a, float2 b, float r) {
  float2 pa = p - a, ba = b - a;
  float h = saturate(dot(pa, ba) / dot(ba, ba));
  return length(pa - ba * h) - r;
}
// Arc [a0,a1]: rotate symmetric about +y, then iq's arc SDF.
float sd_arc(float2 p, float a0, float a1, float r, float ht) {
  float mid = 0.5 * (a0 + a1), ap = 0.5 * (a1 - a0);  // ap = half-aperture
  float2 q = rot2(1.5707963 - mid, p);
  q.x = abs(q.x);
  float2 sc = float2(sin(ap), cos(ap));
  float d = (sc.y * q.x > sc.x * q.y) ? length(q - sc * r)
                                      : abs(length(q) - r);
  return d - ht;
}
```

### 8.4 Fill, stroke, glow compositing

```hlsl
float4 shade_sdf(Inst i, float2 lp) {
  float d   = sd_dispatch(i, lp);                    // meters
  float aa  = max(fwidth(d), 1e-6);                  // analytic AA width
  float cov = saturate(0.5 - d / aa);                // fill coverage
  float4 fill = eval_fill(i, lp);                    // premultiplied
  float sc  = (i.stroke.w > 0.0)
            ? saturate(0.5 - (abs(d) - i.stroke.w * 0.5) / aa) : 0.0;
  float4 col = fill * cov * (1.0 - sc)               // stroke on the edge,
             + float4(i.stroke.rgb, 1.0) * sc;       // over the fill
  if (i.glow_radius > 0.0) {
    float g = exp(-max(d, 0.0) * 4.0 / i.glow_radius);   // 1 inside, decays out
    col.rgb += i.glow.rgb * i.glow.a * g;            // alpha unchanged => additive
  }
  return col;                                        // premultiplied out
}
float4 eval_fill(Inst i, float2 lp) {
  if (i.grad.w == 0.0) return i.fill0;
  float t = (i.grad.w == 1.0)
          ? saturate(dot(lp, i.grad.xy) / i.grad.z + 0.5)   // linear, centered
          : saturate(length(lp) / i.grad.z);                // radial
  return lerp(i.fill0, i.fill1, t);
}
```

The glow trick is load-bearing: glow is written to RGB with **zero added
alpha** — under premultiplied blending that is pure additive light while the
shape body still occludes correctly. Glow intensity ≥ 1 pushes scene values
above 1.0, which is what the bright pass (§12.1) keys on: glow is added
**pre-bloom** and bloom amplifies it.

### 8.5 Batching, static vs dynamic

Per frame, instances append to one CPU array per (layer, blend) bucket, are
uploaded in a single copy pass into a per-frame GPU vertex buffer
(`cycle=true`), and drawn with one `SDL_DrawGPUPrimitives(cmd, 4, count, 0,
first_instance)` per bucket. Never one draw per primitive.

Non-light-bound TBArt primitives are converted to `SdfInstance` once at
`load_table` and live in a static buffer (one copy pass + fence at load).
Light-bound primitives (13-art-direction.md `"light"` field), the ball, and
debug shapes go in the dynamic per-frame buffer with brightness-modulated
colors (§14.3). A layer thus issues at most 2 SDF draws: static + dynamic.

### 8.6 Polygons: ear-clipped meshes (pipelines 3–4)

TBArt `polygon` fills are **not** SDFs. At load, each simple polygon (CCW,
3–256 vertices, validated by tb_validate) is triangulated with standard
O(n²) ear clipping. Mesh vertex layout: `float2 pos` (table meters) +
`ubyte4_norm color` (premultiplied linear). All polygon meshes of a
(layer, blend) bucket share one static vertex/index buffer; one indexed draw
per bucket. Interior fills get no AA; edges that must read crisp carry a
stroke (13-art-direction.md mandates ≥ 1.5 mm on visible polygon edges),
generated at load as CAPSULE instances per edge — which also supplies AA and
glow.

## 9. Pipeline 2: textured sprites (PNG decals, glyphs)

Instanced quads, same slot-0 unit quad.

```c
struct SpriteInstance {               // 80 bytes
  float cx, cy, hx, hy;               // center/half-size, meters or px
  float rot;                          // radians
  uint32_t flags;                     // bit0: 1 = R8 atlas (text), 0 = RGBA
  float u0, v0, u1, v1;               // uv rect (top-left origin)
  float _pad0, _pad1;
  float tint[4];                      // premultiplied linear RGBA
};
```

Fragment: `flags&1 ? tint * atlas.Sample(uv).r : tint * tex.Sample(uv)`
(decals premultiplied at load, so the product stays premultiplied). Batch
key = texture: one draw per (layer, texture, blend). Decals: linear sampler
+ mips (`SDL_GenerateMipmapsForGPUTexture` at load). Font atlas: linear, no
mips.

## 10. Pipeline 3: particles

Instanced quads, **additive** only, drawn as the topmost scene-pass content
(after art layer z 199). One draw call for all live particles.

```c
struct ParticleInstance {             // 48 bytes
  float x, y;                         // table meters
  float size;                         // radius, meters
  float rot;                          // radians (sparks align to velocity)
  float rgba[4];                      // premultiplied linear, faded by life
  float kind;                         // 0 = soft disc, 1 = spark
  float elong;                        // spark length/width ratio (1..8)
  float _pad0, _pad1;
};
```

Fragment (procedural, no textures):

```hlsl
// disc: smooth radial falloff        spark: elongated capsule along local x
float a = (kind == 0) ? pow(saturate(1.0 - length(q)), 2.0)
                      : saturate(1.0 - sd_capsule(q, float2(-e2,0), float2(e2,0),
                                                  1.0/elong) * elong);
return rgba * a;
```

## 11. Ball rendering

The ball is `SdfInstance` kind BALL on the dynamic buffer, one instance per
snapshot ball, radius from the snapshot (canon default 0.0135 m). Shading is
procedural fake-chrome, palette-independent:

```hlsl
float4 shade_ball(float2 lp, float r) {
  float d = length(lp) - r;
  float aa = max(fwidth(d), 1e-6);
  float cov = saturate(0.5 - d / aa);
  float2 pn = lp / r;
  float  nz = sqrt(saturate(1.0 - dot(pn, pn)));
  float3 n = float3(pn, nz);                          // sphere normal
  // Fake chrome: reflect top-down view dir, index a vertical gradient.
  float3 rv = float3(2.0*n.z*n.x, 2.0*n.z*n.y, 2.0*n.z*n.z - 1.0);
  float  t = rv.y * 0.5 + 0.5;                        // up-table component
  float3 env = (t < 0.45) ? lerp(float3(0.05,0.06,0.09), float3(0.13,0.15,0.20), t/0.45)
             : (t < 0.55) ? float3(0.55,0.42,0.30)    // warm horizon band
             :              lerp(float3(0.30,0.34,0.42), float3(0.10,0.12,0.17),
                                 (t-0.55)/0.45);
  // Moving specular: positional light above the table => highlight travels.
  float3 L = normalize(float3(u_light_pos.xy - u_ball_pos.xy, 0.45));
  float3 H = normalize(L + float3(0,0,1));
  float  sp = pow(saturate(dot(n, H)), 48.0) * 1.6;
  float  fres = pow(1.0 - nz, 3.0) * 0.35;            // rim
  float3 c = env * (0.35 + 0.65*nz) + fres * float3(0.5,0.55,0.65) + sp;
  return float4(c, 1.0) * cov;                        // premultiplied
}
```

`u_light_pos = (table_w/2, table_h*0.75)` (fragment uniform, per table);
`u_ball_pos` is the instance center (packed in p1,p2). The light is
positional, so the highlight slides across the ball as it moves — this is
the "moving specular" requirement. Specular deliberately exceeds 1.0 so the
ball blooms slightly.

Ball trail: implemented solely as the `ball_trail` particle effect (§13.4),
enabled per table by art.json `"ball": {"trail": true}` (13-art-direction.md).

## 12. Post-processing chain

Runs entirely on the portrait scene targets. Fullscreen passes use one
3-vertex triangle (`fs_tri.vert`, vertex-id trick, no vertex buffer).

### 12.1 Bright pass (scene_color → bloom0, 1/2 res)

```hlsl
// THRESH = 1.0, KNEE = 0.5 (config keys render.bloom_threshold / .bloom_knee)
float3 c   = scene.Sample(s, uv).rgb;
float  lum = dot(c, float3(0.2126, 0.7152, 0.0722));
float  knee = THRESH * KNEE;
float  soft = clamp(lum - THRESH + knee, 0.0, 2.0 * knee);
soft = soft * soft / (4.0 * knee + 1e-5);
float  w = max(soft, lum - THRESH) / max(lum, 1e-5);
return float4(c * w, 1.0);
```

Only glow, specular, and additive stacks exceed 1.0, so ordinary art does
not bloom — bloom is earned by glow, not free.

### 12.2 Downsample (bloom0 → bloom1 → bloom2)

4-tap box: sample at uv + (±0.5, ±0.5) source texels, average. One pass per
level.

### 12.3 Separable Gaussian (each level, ping-pong)

9-tap, σ = 2.0 source texels: horizontal into `blurN`, vertical back into
`bloomN`. Direction is fragment uniform `float2 u_dir` (texel step).
Weights (offsets 0..4): `0.227027, 0.194595, 0.121622, 0.054054, 0.016216`.

```hlsl
float3 acc = src.Sample(s, uv).rgb * W[0];
for (int i = 1; i <= 4; i++) {
  acc += src.Sample(s, uv + u_dir * i).rgb * W[i];
  acc += src.Sample(s, uv - u_dir * i).rgb * W[i];
}
```

### 12.4 Upsample + additive composite of levels

`bloom2` draws additively into `bloom1` (bilinear upsample, pipeline 11),
then `bloom1` into `bloom0`; `bloom0` then holds the combined 3-level bloom.
Totals: 1 bright + 2 downsample + 6 blur + 2 upsample = 11 post passes, all
at ≤ 1/2 res. This chain is fixed in v1; do not add levels.

### 12.5 Final composite (→ swapchain)

Draws the rotated letterbox quad (§6.4) after clearing the swapchain to
black. `BLOOM_STRENGTH = 0.6` default (config `render.bloom_strength`,
range 0–2).

```hlsl
float3 c = scene.Sample(s, uv).rgb + BLOOM_STRENGTH * bloom0.Sample(s, uv).rgb;
// Saturation clamp: overflow desaturates toward white, no hue shift.
float m = max(c.r, max(c.g, c.b));
if (m > 1.0) c = lerp(c / m, float3(1,1,1), saturate((m - 1.0) * 0.35));
c = clamp(c, 0.0, 1.0);
// ---- optional CRT branch (below); skipped entirely when u_crt == 0 ----
if (u_crt != 0.0) {
  float2 spx  = uv * u_scene_px;                     // scene (logical) pixels
  float  scan = (fmod(floor(spx.y), 3.0) < 1.0) ? 1.0 : 0.0;  // 1 row in 3
  c *= 1.0 - 0.12 * scan;                            // scanline: 0.88x that row
  float2 q = uv * 2.0 - 1.0;                         // -1..1 across the image
  float  r = length(q) * 0.70710678;                 // 0 center, 1 at a corner
  c *= 1.0 - 0.15 * smoothstep(0.6, 1.0, r);         // vignette
}
return float4(srgb_encode(c), 1.0);      // exact piecewise sRGB
```

Tuning procedure for `BLOOM_STRENGTH`/`THRESH`: render test-lab's
`bloom-grid` art sheet (glow intensities 0–2 in 0.25 steps) via
`tb_screenshot`; accept when (a) intensity-0 shapes show zero bloom,
(b) intensity-2 white glow reads as a coherent halo ≤ 3 × glow_radius wide,
(c) no visible banding. Locked by the SSIM golden-image test
(16-testing-ci.md).

**Optional CRT scanlines + vignette.** This document is the **implementation
owner** of the CRT effect; 13-art-direction.md §10 owns its look and supplies
the two formulas restated verbatim in the branch above.

- **Setting.** `render.crt` (05-engine-core.md §11.1), boolean, **default
  `false`**. User setting only — a table can never enable it (13 §10). It is
  read at frame build time into the fragment uniform `float u_crt` (1.0/0.0),
  so toggling it in the M18 settings menu takes effect on the next frame with
  no pipeline or target rebuild.
- **Where.** Inside the pass-F fragment shader (`composite.frag`), after the
  bloom mix and the saturation clamp, **after** the `clamp(c, 0, 1)` and
  immediately **before** `srgb_encode` — i.e. "after bloom, before sRGB
  encode" per 13 §10. Applying it before the clamp would hide scanlines
  wherever the scene is overbright (0.88 × 2.0 still clamps to 1.0), which is
  exactly where a CRT shows them most.
- **Not a shader variant.** It is a runtime *uniform* branch (coherent across
  the whole draw), not a `-D` permutation: §16.4 already doubles
  swapchain-target fragment shaders for `TB_ENCODE_SRGB`, and a second axis
  would double the committed ADR-012 blobs again for no gain.
- **Uniforms.** The composite pass's fragment uniform block (b1, space3,
  pushed with `SDL_PushGPUFragmentUniformData(cmd, 1, ...)`) carries
  `BLOOM_STRENGTH` plus `float u_crt` and `float2 u_scene_px` =
  `(scene_w, scene_h)` from §6.2.
- **Pixel space.** `spx` is in **scene/logical** pixels (the letterbox quad
  maps the scene target 1:1 onto logical pixels, §6.2/§6.4), never swapchain
  pixels. Scanlines therefore run across the image as the *player* sees it on
  a rotated panel, and because `rot ∈ {0,90,180,270}` the logical pixel grid
  maps exactly onto physical pixels — a 3-px period stays 3 px, with no
  resampling and no moiré.
- **Numbers.** Scanlines darken one row in three to 0.88×, i.e. a mean
  luminance of `(0.88 + 1 + 1)/3 = 0.96` (−4.0%). The vignette is elliptical
  in `uv`: `r = 0` at the image center, `1` at a corner, so it begins at
  `r = 0.6` (= 84.9% of the way to an edge: for the reference 960 × 1920
  scene, within 72.7 px of the left/right edge or 145.4 px of the top/bottom
  edge), reaches only 0.9735× (−2.7%) at an edge midpoint (`r = 0.7071`), and
  0.85× (−15%) in the corners. Worst case, a corner scanline row is
  `0.85 × 0.88 = 0.748` (−25.2%). Both factors scale the whole linear RGB
  triple, which is exactly "multiply luminance" (13 §10) with no hue shift.
- **Nothing else changes.** No barrel distortion, no chromatic aberration, no
  phosphor mask (13 §10 forbids all three — they blur the ball). Pass G
  (debug draw + F1 overlay) and the backglass draw straight to their
  swapchains after/outside pass F, so overlays and the backglass are never
  scanlined.
- **Cost when enabled.** ~12 extra fragment ALU ops per shaded pixel of the
  letterbox quad only (960 × 1920 = 1.84 M pixels on the reference cabinet;
  the black bars are cleared, never shaded) inside an existing pass: no
  extra pass, no
  extra render target, no extra draw call, no extra VRAM — post passes stay
  at 11 + composite (§17.1). Budget: **≤ 0.1 ms GPU** on the reference iGPU;
  enabling it must still fit the 4.0 ms §17.1 GPU budget. With `render.crt`
  false the branch is uniform and costs nothing measurable, so the perf gate
  (16-testing-ci.md) runs at the default.
- **Goldens.** `tb_screenshot` always composites with CRT **off**, for the
  same reason particles are always skipped (§15.2): golden images must not
  depend on a user setting. F12 in-app screenshots (§15.1) re-render the
  composite and therefore *do* include CRT when it is on.

## 13. Particle system (CPU)

### 13.1 Pool

Fixed SoA pool, capacity **8192** (compile-time `TB_MAX_PARTICLES`): arrays
pos, vel, life, life0, size0/1, color0/1, kind, elong, drag, grav. When
full, new spawns steal the oldest live particle. Simulated on the **main
thread at render rate** with the frame wall dt (clamped ≤ 33 ms). Particles
are visual-only and non-deterministic; they must never feed back into the
sim (canon §5.3). RNG: a render-owned PCG32 seeded from the wall clock —
**not** `tb.rng`.

```
vel += (0, -grav) * dt          # grav pulls down-table (visual gravity)
vel *= 1 / (1 + drag * dt)
pos += vel * dt
life -= dt;  t = 1 - life/life0
size  = lerp(size0, size1, t)
color = lerp(color0, color1, t) * min(1, life/0.1)   # tail fade-out
```

### 13.2 Spawning from SimEvents

Each render frame drains the SimEvent ring (05-engine-core.md) and maps
events to effects at the event's table-space position:

| SimEvent | Effect |
|---|---|
| bumper contact | bumper_hit_burst |
| slingshot fire | sling_flash (cone along sling normal from payload) |
| ball on ramp (per tick, rate-limited) | ramp_trail |
| ball speed > 2 m/s (from snapshot) | ball_trail (if art.json enables it) |
| drain | drain_burst |
| magnet active (while on) | magnet_sparks |
| tilt_warning | tilt_warning_flash |
| EffectRequest{name, x, y} | named effect (scripted; e.g. jackpot_starburst) |

`EffectRequest` is a generic sim event emitted by sim elements and by the
scripting layer's light-show API (surface owned by 10-scripting.md). Unknown
names log once and no-op.

### 13.3 Emitter parameters

An effect is a row of: `burst` count or `rate` particles/s; `shape` (point,
ring(r), cone(dir, spread_deg)); `speed` [min,max] m/s uniform; `life`
[min,max] s; `size` start→end m; `color` start→end palette role (resolved
per table, 13-art-direction.md) × brightness; `kind` disc|spark; `grav`
m/s² down-table; `drag` 1/s.

### 13.4 Canonical effects (binding values)

| Effect | burst/rate | shape | speed m/s | life s | size mm | color (role) | kind | grav | drag |
|---|---|---|---|---|---|---|---|---|---|
| bumper_hit_burst | 24 | ring r=0.02 | 0.8–1.6 | 0.25–0.45 | 4→1 | accent1→primary | spark | 0 | 3.0 |
| sling_flash | 12 | cone spread 60° | 1.0–2.0 | 0.20–0.35 | 5→1.5 | warm→primary | disc | 0 | 2.0 |
| ramp_trail | rate 120/s | point | 0.1–0.3 (opposite ball vel) | 0.30–0.60 | 3→0.5 | secondary→secondary×0 | disc | 0 | 4.0 |
| ball_trail | rate 90/s | point | 0.05–0.15 | 0.15–0.30 | 5→1 | glow_white×0.35→×0 | disc | 0 | 6.0 |
| drain_burst | 40 | cone up-table, spread 40° | 0.5–1.5 | 0.40–0.80 | 4→1 | warm→bg1 | spark | 3.0 | 1.0 |
| jackpot_starburst | 96 | ring r=0.01 | 1.5–3.0 | 0.60–1.00 | 6→1 | glow_white→accent2 | spark | 0 | 2.5 |
| magnet_sparks | rate 200/s | ring r=0.02, inward | 0.3–0.8 | 0.10–0.25 | 2→0.5 | accent1→accent1×0 | spark | 0 | 0.5 |
| tilt_warning_flash | 30 | ring r=0.03 at nudge point | 0.4–1.0 | 0.30–0.50 | 5→1 | warm→warm×0 | disc | 0 | 2.0 |

Spark rotation = atan2(vel); elong = clamp(speed / 0.5, 1, 8). Spawn
brightness 1.4 (so bursts bloom). Flash-reduction mode (13-art-direction.md
§Accessibility) halves jackpot_starburst's count and applies brightness
×0.6 to all effects, at spawn time.

### 13.5 Flash overlay

`tilt_warning` and scripted flashes set a fullscreen flash in `RenderFrame`:
one additive RBOX covering the table, color = palette `warm`, intensity
0.25, decaying linearly over 200 ms. Flash-reduction halves its intensity.

## 14. Light-insert rendering and text

### 14.1 Font atlas

Baked once at startup with stb_truetype (`stbtt_PackBegin`) into the single
2048×2048 R8 atlas: Orbitron-Bold, Monoton-Regular, Righteous-Regular
(vendored OFL, /assets/fonts — 13-art-direction.md), each at pixel sizes
**24, 48, 96**, codepoints U+0020–U+007E and U+00A0–U+00FF (ASCII +
Latin-1); 24 px uses 2×2 oversampling. 9 ranges; a pack failure aborts (it
fits with ≥ 15% slack — failure means a corrupt font). Glyphs render via the
sprite pipeline (flags bit0 = 1). Size selection: smallest baked size ≥
target pixel height; upscaling beyond 1.15× is forbidden — take the next
size up.

### 14.2 Segmented score digits (backglass)

Backglass scores use a 14-segment style drawn as CAPSULE SDF instances
(pipeline 13, pixel space, glow_radius = 0.35 × cell width). Cell normalized
[0,1]×[0,1], y up, thickness 0.09, optional 8° italic skew
(`x += 0.14 * y` before scaling). Endpoints:

```
        A                    A : (0.10,0.97)-(0.90,0.97)
      -----                  B : (0.95,0.92)-(0.95,0.55)
   F | H I J | B             C : (0.95,0.45)-(0.95,0.08)
      -- --                  D : (0.10,0.03)-(0.90,0.03)
     G1    G2                E : (0.05,0.45)-(0.05,0.08)
   E | K L M | C             F : (0.05,0.92)-(0.05,0.55)
      -----                  G1: (0.10,0.50)-(0.45,0.50)  G2: (0.55,0.50)-(0.90,0.50)
        D                    H : (0.13,0.90)-(0.42,0.57)  I : (0.50,0.92)-(0.50,0.55)
                             J : (0.87,0.90)-(0.58,0.57)  K : (0.13,0.10)-(0.42,0.43)
                             L : (0.50,0.45)-(0.50,0.08)  M : (0.87,0.10)-(0.58,0.43)
```

Bit masks (bit0=A, 1=B, 2=C, 3=D, 4=E, 5=F, 6=G1, 7=G2, 8=H, 9=I, 10=J,
11=K, 12=L, 13=M):

| Char | Segments | Mask |
|---|---|---|
| 0 | A B C D E F J K | 0x0C3F |
| 1 | B C | 0x0006 |
| 2 | A B G1 G2 E D | 0x00DB |
| 3 | A B C D G2 | 0x008F |
| 4 | F G1 G2 B C | 0x00E6 |
| 5 | A F G1 G2 C D | 0x00ED |
| 6 | A F E D C G1 G2 | 0x00FD |
| 7 | A B C | 0x0007 |
| 8 | A B C D E F G1 G2 | 0x00FF |
| 9 | A B C D F G1 G2 | 0x00EF |

Only digits, space, and comma (small capsule below the baseline right of the
cell) are supported; anything else falls back to Orbitron atlas text. Lit
segments use the color assigned by 13-art-direction.md; **unlit segments
draw as ghosts at 6% brightness, no glow**.

### 14.3 Light inserts

The snapshot carries per-light state published by the sim
(05-engine-core.md):

```c
struct LightState {
  uint8_t  mode;            // 0 off, 1 on, 2 blink_square, 3 breathe_sine, 4 chase
  uint8_t  duty_pct;        // square duty, 0-100
  uint16_t rate_chz;        // rate in centihertz (200 = 2.00 Hz)
  uint32_t start_tick;      // sim tick when the pattern started
  uint8_t  chase_index, chase_len;
  uint8_t  brightness_pct;  // master, set by script (default 100)
};
```

The sim stores *numeric* parameters; named patterns (`slow_blink` 2 Hz etc.)
are resolved to numbers by the scripting layer from the normative timing
table in 10-scripting.md §3.2 (usage guidance in 13-art-direction.md §7.1)
— the renderer never sees pattern names.
**Brightness is evaluated on the CPU each frame** (decision: not GPU —
light counts are ≤ 256 and CPU evaluation keeps shaders uniform-free with
blink phase locked to sim time):

```
t = (snapshot.tick - start_tick) * 0.001                 # seconds, sim time
square:  b = frac(t * rate) < duty ? 1 : 0
breathe: b = 0.15 + 0.85 * (0.5 + 0.5 * sin(2*pi*rate*t))
chase:   slot = floor(frac(t * rate) * chase_len)
         b = (slot == chase_index) ? 1 : max(0, 0.3 - dist_in_group * 0.15)
b *= brightness_pct / 100
```

Using `snapshot.tick`, never the wall clock, keeps blinking identical across
replays and screenshots. Flash-reduction clamps `rate` to ≤ 3 Hz and raises
duty to ≥ 50% at evaluation time (render-side only; sim untouched).

Every TBArt primitive bound via `"light": "<id>"` enters the dynamic SDF
buffer each frame with `fill *= b`, `glow.a *= b`, floored at 15% fill so
unlit inserts stay readable (13-art-direction.md). Inserts under the ball
sit at z < 100; wire/ramp lights at z ≥ 100.

### 14.4 Snapshot usage

The renderer always draws the **latest complete snapshot**; at 1000 Hz it is
at most ~1 ms stale, so no interpolation or extrapolation is performed
(decision: interpolation would add a tick of latency for no visible benefit).

## 15. Screenshots and tb_screenshot

### 15.1 In-app (F12)

`request_screenshot(path)` sets a pending flag. Next playfield frame, after
pass G, the composite output is re-rendered into the `readback` RGBA8 target
(one extra fullscreen draw — do **not** copy the swapchain texture; swapchain
textures are not guaranteed copy sources on all SDL GPU backends), then a
copy pass runs `SDL_DownloadFromGPUTexture` into an `SDL_GPUTransferBuffer`.
After that frame's fence signals, map and write with `stbi_write_png` (row 0
is the top row — no flip; respect `pixels_per_row` pitch). Default path:
`<SDL_GetPrefPath>/screenshots/tiltburst_YYYYMMDD_HHMMSS.png` (wall clock is
fine — render side only).

### 15.2 tb_screenshot (offscreen tool)

This section is the **normative CLI contract** for `tb_screenshot`;
04-milestones.md M15 and 14-authoring-guide.md §8.5 cite it and must not
restate different flags or exit codes.

```
tb_screenshot <table-dir> --out <png-or-dir> [--width 1080] [--height 1920]
              [--tick T] [--seed S] [--replay in.tbreplay] [--art-only]
              [--views full,lower,upper,backglass,attract]
              [--state <mode-id|multiball|wizard>]
```

Behavior: `SDL_Init(SDL_INIT_VIDEO)`; create a 64×64 **hidden** window
(`SDL_WINDOW_HIDDEN` — some drivers refuse device creation with no window);
create the device per §3 but claim no swapchain; load the table; step the
headless sim to tick `T` (default 0) with seed `S` (default 1) or play the
replay (`.tbreplay`, the format `tb_autoplay --replay` records); render one
frame into an offscreen RGBA8 target of `--width`×`--height` (portrait, no
rotation, letterboxed per §6.2 with the target as the logical viewport);
download; write PNG; exit 0. `--art-only` skips sim stepping and draws art
layers + unlit inserts only. Particles are always skipped in tb_screenshot
(wall-clock animated — they would break golden images).

Authoring-review extensions (used by 14-authoring-guide.md §8.5): `--views`
selects one or more images — `full` (default; whole playfield), `lower`
(bottom third), `upper` (top third, or layer 1 if present), `backglass`,
`attract` (attract light cycle, 3 frames); `--state` renders the named
mode's light state. When more than one image results, `--out` names a
directory and each image is written inside it as `<view>.png`
(`attract_0.png`–`attract_2.png` for attract).

### 15.3 No-GPU behavior (binding)

If `SDL_Init` or `SDL_CreateGPUDevice` fails, tb_screenshot prints
`tb_screenshot: no GPU device available: <SDL_GetError()>` to stderr and
exits **2**. The exit-code table is part of the §15.2 normative contract:
**0** = success, **1** = bad arguments, table load, or render error,
**2** = no GPU. CI treats exit 2 as "skipped" (16-testing-ci.md); Linux CI
installs Mesa llvmpipe/lavapipe so golden-image jobs actually run.

## 16. Debug draw, overlays, shader build

### 16.1 F2 debug draw

F2 cycles: off → colliders → colliders+broadphase → everything. Drawn in
pass G (post-bloom, no glow) via pipeline 13, table-space coordinates
through the §6.4 transform. Line width = `1.5 / ppm` meters (1.5 px). Colors
(sRGB): collider segments `#00FF66`; collider arcs/circles `#00CCFF`;
contact points `#FFFF00` (2 px discs) with normal ticks `#00FFFF` (5 px);
ball velocity `#FF00FF` scaled 0.05 s (v×0.05 m long); broadphase grid
`#FFFFFF` at 8% alpha, occupied cells 25%; flipper sweep arcs `#FF8800`.
Data comes from the snapshot's debug section (08-physics.md publishes it
when enabled).

### 16.2 F1 overlay and counters

F1 toggles the stats overlay (Orbitron 24 px, top-left, pixel space): the
timing ring from 05-engine-core.md plus `RenderStats`, averaged over 120
frames:

```
fps / frame ms (avg, p99)      sim tick ms (avg, max)
acquire wait ms                cpu encode ms
gpu ms (fence, §17.2)          draw calls (scene/post/ui)
sdf instances  sprites  particles live/spawned
bloom res      vram est MB     backglass fps
```

### 16.3 Shader source layout

```
/shaders/
  fs_tri.vert.hlsl      sdf.vert.hlsl        sdf.frag.hlsl
  mesh.vert.hlsl        mesh.frag.hlsl       sprite.vert.hlsl
  sprite.frag.hlsl      particle.vert.hlsl   particle.frag.hlsl
  brightpass.frag.hlsl  downsample.frag.hlsl blur.frag.hlsl
  upsample.frag.hlsl    composite.vert.hlsl  composite.frag.hlsl
  compiled/             # committed fallback blobs (ADR-012)
```

Entry point is always `main`. SDL GPU HLSL register conventions: vertex —
textures/samplers `space0`, uniforms `space1`; fragment — textures/samplers
`space2`, uniforms `space3`.

### 16.4 Build (SDL_shadercross) and fallback (ADR-012)

CMake fetches SDL_shadercross via FetchContent and adds one custom command
per (source, format):

```cmake
foreach(fmt IN ITEMS spv dxil msl)
  add_custom_command(
    OUTPUT  ${SHADER_OUT_DIR}/${name}.${fmt}
    COMMAND shadercross ${src} -o ${SHADER_OUT_DIR}/${name}.${fmt}
            $<$<BOOL:${defines}>:-D${defines}>
    DEPENDS ${src})
endforeach()
```

Stage is inferred from the `.vert.hlsl`/`.frag.hlsl` suffix; output format
from the destination extension. Swapchain-target fragment shaders are
compiled twice (`-DTB_ENCODE_SRGB=0` variant, §5). Outputs install to
`<binary_dir>/shaders/` next to the executable.

- DXIL requires DXC with `dxil.dll` (Windows CI only). Each platform
  compiles the formats it can; at runtime the renderer scans `shaders/` and
  passes only formats actually present to `SDL_CreateGPUDevice` (§3), so SDL
  picks a compatible backend (e.g. Vulkan on Windows if DXIL is absent).
- **ADR-012 fallback:** `/shaders/compiled/` holds committed
  `.spv/.dxil/.msl` blobs, refreshed by Windows CI whenever HLSL changes
  (CI check: recompile and diff). CMake option `TB_COMPILE_SHADERS`
  (ADR-012; default ON, forced OFF when shadercross is unavailable)
  toggles the compile step; with it OFF the committed blobs are installed
  verbatim instead of compiling.
- Shader loading uses a hardcoded C++ manifest per shader — entry name,
  stage, sampler count, uniform-buffer count, matching this spec's
  pipelines. No runtime reflection.

## 17. Performance budgets

### 17.1 Budgets (binding; gated in 16-testing-ci.md perf jobs)

Reference iGPU class: Intel UHD 730/770 or AMD Vega 8 at 1080×1920.

| Metric | Budget | Typical target |
|---|---|---|
| Playfield draw calls / frame | ≤ 200 | ≤ 60 |
| Playfield GPU time / frame | ≤ 4.0 ms | ≤ 2.5 ms |
| CPU encode time / frame | ≤ 1.5 ms | ≤ 0.8 ms |
| SDF instances / frame | ≤ 6000 | ≤ 2500 |
| Particles live | ≤ 8192 (pool) | ≤ 3000 |
| Post passes | 11 + composite (fixed) | — |
| VRAM (targets + atlas + decals) | ≤ 256 MB | ≤ 96 MB |
| Backglass draw calls / frame | ≤ 40 | ≤ 20 |

Exceeding a budget is a CI failure, not a note. If GPU time exceeds 4 ms the
mandated order of cuts: bloom base to 1/4 res → particle pool 4096 → glow
pad 1.0×. Never drop below 60 fps (canon R1).

### 17.2 Measuring GPU time

SDL GPU has no timestamp queries. GPU time = wall time between
`SDL_SubmitGPUCommandBufferAndAcquireFence` and the first
`SDL_QueryGPUFence` poll returning true (polled at the top of each frame,
§4.1; then released). With frames-in-flight 1 this upper-bounds real GPU
time within one frame's granularity — sufficient for the gate. Shown as
"gpu ms (fence)" in the F1 overlay.

## Common pitfalls

- **Straight-alpha blending.** Everything is premultiplied linear; using
  `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` gives dark fringes on glow edges. Correct:
  blend states exactly as §7, premultiply at load.
- **Bloom or gradients in gamma space.** Muddy, clipped glow. Correct:
  linearize at load, RGBA16F intermediates, sRGB-encode only in composite
  (§5).
- **Blocking backglass acquire.** `SDL_WaitAndAcquireGPUSwapchainTexture` on
  the backglass pins the playfield to the backglass vsync. Correct:
  non-blocking acquire, cancel + skip on NULL (§4.2).
- **Evaluating blink or shader time on the wall clock.** Breaks replays and
  golden screenshots, drifts against sim-driven sound. Correct: all time
  derives from `snapshot.tick * 0.001` (§6.5, §14.3).
- **One draw call per primitive.** Correct: per-(layer, blend) instance
  buckets; static art uploaded once at load (§8.5).
- **Unpadded glow quads.** Quad sized to the shape clips glow into a visible
  square. Correct: half-extent includes stroke/2 + 1.25 × glow_radius (§8.1).
- **Rotating via OS display rotation or a rotated viewport.** Costs a
  compositor pass and breaks letterbox math. Correct: rotation exists only
  in the composite corner transform (§6.4); all earlier passes are portrait.
- **Interpolating snapshots or reading sim state directly.** Only the
  published snapshot pointer is touched; nothing to interpolate at 1000 Hz
  (§14.4).
- **Copying the swapchain texture for screenshots.** Not a legal copy source
  on all backends. Correct: re-render composite into the readback target
  (§15.1).
- **Forgetting `cycle=true` on per-frame uploads.** GPU stalls or torn
  instance data with FIF 1. Correct: cycle every per-frame transfer and
  vertex buffer.
- **Assuming DXIL exists everywhere.** Linux/macOS cannot run DXC. Correct:
  device format mask = formats present on disk (§16.4).
- **Building the CRT effect as an extra pass, a shader permutation, or in
  swapchain pixels.** A separate fullscreen pass costs a target and a blit; a
  `-D` variant doubles the committed ADR-012 blobs again; swapchain-space
  scanlines turn with the panel instead of the image. Correct: one uniform
  `u_crt` branch inside `composite.frag`, after the `[0,1]` clamp, addressed
  in scene/logical pixels (§12.5).

## Done when

- [ ] Device created with `debug_mode = cli.dev || TB_DEV_BUILD` (on in dev
      builds, and forced on by `--dev` in a Release build), matching 05 §1
      step 9; both windows claimed; SDR composition; MAILBOX-else-VSYNC on
      the playfield; frames-in-flight 1 — all verified in the log.
- [ ] Playfield holds native refresh with the backglass enabled; forcing the
      backglass display to 30 Hz does not change playfield frame times (F1
      overlay, 07-displays.md test plan).
- [ ] All GPU work happens on the main thread (dev-build thread-id assert in
      every `IRenderer` entry point).
- [ ] `renderer.h` contains no SDL_GPU includes; `tb_game`/`tb_table` link
      without seeing any backend type.
- [ ] Rotations 0/90/180/270 letterbox correctly on 1920×1080 and 1080×1920
      modes (4 golden screenshots of test-lab match).
- [ ] SDF pipeline renders circle, ring, rbox, capsule, arc with stable AA
      (no shimmer in the test-lab primitive sheet), stroke, 2-stop
      gradients, and exponential glow surviving the 1.25× pad.
- [ ] A concave 12-vertex polygon in test-lab ear-clips at load and renders
      with edge strokes.
- [ ] Ball shows radial shading, horizon-band chrome, and a specular
      highlight that moves as the ball crosses the table.
- [ ] Bloom chain is exactly §12 (11 post passes + composite); glow
      intensity 0 produces zero bloom; overflow desaturates toward white.
- [ ] `render.crt` defaults false and is the only switch for the §12.5 CRT
      branch (no table can set it): with it on, one scene row in three is
      0.88× and the corners 0.85× (13 §10 formulas), the pass/draw-call
      counts in the F1 overlay are unchanged, GPU time rises ≤ 0.1 ms, and
      the F1 overlay and backglass stay un-scanlined; tb_screenshot goldens
      are byte-identical with the setting on or off.
- [ ] All 8 canonical particle effects fire from their SimEvents with §13.4
      values; pool never exceeds 8192; overflow steals oldest.
- [ ] Light blinking is phase-locked to sim ticks: one replay run twice
      yields bit-identical brightness sequences.
- [ ] Font atlas bakes 3 fonts × 3 sizes + Latin-1 into one 2048² R8 texture
      at startup; 14-segment digits render with 6% ghost segments.
- [ ] F1 shows all §16.2 counters; F2 cycles the three debug layers.
- [ ] F12 writes a correct PNG; tb_screenshot yields a deterministic golden
      image for test-lab (same seed ⇒ identical PNG on one machine) and
      exits 2 with the documented message when no GPU exists.
- [ ] Shaders build via SDL_shadercross on all 3 OSes; a clean build with
      `TB_COMPILE_SHADERS=OFF` and no shadercross also boots.
- [ ] Perf gate on reference iGPU: Neon Drift beauty pass ≤ 200 draw calls,
      GPU ≤ 4 ms, CPU encode ≤ 1.5 ms (F1, 120-frame average).
