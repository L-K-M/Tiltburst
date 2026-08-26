#pragma once

#include "sim/collider.h"
#include "sim/snapshot.h"

#include <cstdint>
#include <memory>

struct SDL_Window;

// Renderer boundary (06-rendering.md §2, ADR-005). This is the only render
// header included outside tb_render, and it must not include or mention
// any SDL_GPU* type. SDL_Window* is permitted (a Vulkan backend consumes
// it too). GPU concepts cross as POD structs; the game layer builds a
// RenderFrame description and the renderer consumes it. No callbacks into
// game code, no immediate-mode drawing API.
namespace tb::render {

enum class Rotation : int8_t { ROT_0 = 0, ROT_90 = 1, ROT_180 = 2, ROT_270 = 3 };

struct TextureHandle {
    uint32_t id = 0; // 0 = invalid
};

struct RendererConfig {
    SDL_Window* playfield_window = nullptr; // required
    SDL_Window* backglass_window = nullptr; // nullptr => single-display mode
    Rotation playfield_rotation = Rotation::ROT_0;
    bool debug_device = false;  // SDL GPU debug mode
    bool prefer_mailbox = true; // canon §5.4
};

struct RenderStats; // §17

// Grows at M3/M5: parsed TBArt document, ear-clipped meshes, decals,
// light bindings (06 §8.6, §9).
struct TableRenderData {};

// Grows at M9/M12: backglass draw list (scores, messages).
struct BackglassFrame {};

// One flat-tint quad in target pixel space (cx, cy center; hx, hy
// half-extents) with premultiplied color. The overlay/debug paths fill
// these; the pipeline is the M1 sprite pair (untextured flat tint).
struct QuadInstance {
    float cx, cy, hx, hy;
    float r, g, b, a;
};

// Per-frame description (06 §2). M3 adds the F2 debug-draw inputs; TBArt,
// text runs, particles, and flash overlay arrive at M13.
struct RenderFrame {
    const SimSnapshot* snapshot = nullptr;
    float wall_dt = 0.0f;
    const QuadInstance* quads = nullptr;
    uint32_t quad_count = 0;
    bool show_overlay = true;

    // F2 debug draw (06 §16.1): colliders from the current table scene.
    const sim::Collider* debug_colliders = nullptr;
    uint32_t debug_collider_count = 0;
    bool show_colliders = false;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(const RendererConfig&) = 0;
    virtual void shutdown() = 0;
    virtual bool load_table(const TableRenderData&) = 0; // §8.6, §9
    virtual void unload_table() = 0;
    // Per frame, main thread only (canon §5.4):
    virtual void render_playfield(const RenderFrame&) = 0;
    virtual bool render_backglass(const BackglassFrame&) = 0;  // false = skipped
    virtual void request_screenshot(const char* png_path) = 0; // §15
    virtual const RenderStats& stats() const = 0;
};

std::unique_ptr<IRenderer> make_sdl_gpu_renderer(); // only factory in v1

} // namespace tb::render
