#pragma once

#include "render/pipeline_quad.h"
#include "render/present_pass.h"
#include "render/renderer.h"
#include "render/sdf_batch.h"
#include "render/view.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

// SDL3 GPU implementation of IRenderer (the only backend in v1, ADR-005).
// All GPU work happens on the main thread (canon §5.4).
//
// M3 frame shape (06 §4 reduced): pass A' draws the scene target (clear +
// SDF debug instances + overlay quads, portrait); pass F' presents the
// scene as the rotated letterbox quad with piecewise sRGB encode. The
// bloom chain and TBArt arrive at M13.
namespace tb::render {

struct RenderStats {
    uint32_t frames = 0;
    float frame_ms_avg = 0.0f;
    uint32_t playfield_skips = 0;
    uint32_t sdf_instances = 0;
};

class SdlGpuRenderer final : public IRenderer {
public:
    SdlGpuRenderer() = default;
    ~SdlGpuRenderer() override;
    SdlGpuRenderer(const SdlGpuRenderer&) = delete;
    SdlGpuRenderer& operator=(const SdlGpuRenderer&) = delete;

    bool init(const RendererConfig&) override;
    void shutdown() override;

    bool load_table(const TableRenderData&) override { return true; }

    void unload_table() override {}

    void render_playfield(const RenderFrame&) override;
    bool render_backglass(const BackglassFrame&) override;
    void request_screenshot(const char* png_path) override;

    const RenderStats& stats() const override { return stats_; }

    // Offscreen smoke path (--render-smoke): device without a window.
    bool init_offscreen(uint32_t width, uint32_t height, bool debug_mode);
    bool render_offscreen_frame(const RenderFrame&);
    bool write_png(const std::filesystem::path& out_png);
    std::string backend_name() const;

private:
    // Scene target management: recreated whenever the computed scene size
    // changes (letterbox_changed, §6.3).
    bool ensure_scene(SDL_GPUTextureFormat scene_format);
    void destroy_device();

    // Draws one frame's content into `scene` (portrait space).
    void draw_scene(SDL_GPUCommandBuffer* cmd, const RenderFrame& frame, double time_s);

    // F2 debug instance builders (16.1).
    void collider_instances(const sim::Collider& c, float ppm, std::vector<SdfInstance>& out);
    void ball_instances(const SimSnapshot& snap, std::vector<SdfInstance>& out);

    // Re-renders the current content into an offscreen RGBA8 target and
    // downloads it (F12 path — never copies the swapchain, §15.1).
    bool capture_to_png(const std::filesystem::path& png);

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* playfield_ = nullptr;
    SDL_Window* backglass_ = nullptr;
    Rotation rotation_ = Rotation::ROT_0;

    QuadBatch quads_;
    SdfBatch sdf_;
    PresentPass present_;
    ViewTransform view_{};

    SDL_GPUTexture* scene_ = nullptr; // RGBA16F (§7)
    Uint32 scene_w_ = 0;
    Uint32 scene_h_ = 0;

    // Offscreen smoke target (--render-smoke).
    SDL_GPUTexture* offscreen_ = nullptr;
    SDL_GPUTransferBuffer* smoke_readback_ = nullptr;
    Uint32 offscreen_w_ = 0;
    Uint32 offscreen_h_ = 0;

    // F12 capture targets (created on demand).
    SDL_GPUTexture* capture_ = nullptr;
    SDL_GPUTransferBuffer* readback_ = nullptr;
    Uint32 capture_w_ = 0;
    Uint32 capture_h_ = 0;

    RenderStats stats_;
    std::deque<float> frame_ms_;
    std::string pending_screenshot_;

    std::vector<QuadInstance> overlay_quads_;
    std::vector<SdfInstance> debug_instances_;

    // Cached last frame for the F12 offscreen re-render (15.1).
    RenderFrame last_{};
    std::vector<QuadInstance> last_quads_;
    SimSnapshot last_snapshot_{};

    friend class SdlGpuRendererTestPeer;
};

} // namespace tb::render
