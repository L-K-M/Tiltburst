#pragma once

#include "render/pipeline_quad.h"
#include "render/renderer.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

// SDL3 GPU implementation of IRenderer (the only backend in v1, ADR-005).
// All GPU work happens on the main thread (canon §5.4).
namespace tb::render {

struct RenderStats {
    uint32_t frames = 0;
    float frame_ms_avg = 0.0f;
    uint32_t playfield_skips = 0;
};

class SdlGpuRenderer final : public IRenderer {
public:
    bool init(const RendererConfig&) override;
    void shutdown() override;

    bool load_table(const TableRenderData&) override { return true; } // M3

    void unload_table() override {}

    void render_playfield(const RenderFrame&) override;
    bool render_backglass(const BackglassFrame&) override;
    void request_screenshot(const char* png_path) override; // M3 wires capture

    const RenderStats& stats() const override { return stats_; }

    // Offscreen smoke path (--render-smoke): device without a window,
    // frames into an RGBA8 target, PNG readback of the last frame.
    bool init_offscreen(uint32_t width, uint32_t height, bool debug_mode);
    bool render_offscreen_frame(const RenderFrame&);
    bool write_png(const std::filesystem::path& out_png);
    std::string backend_name() const;

private:
    void destroy_device();

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* playfield_ = nullptr;
    SDL_Window* backglass_ = nullptr;
    Rotation rotation_ = Rotation::ROT_0;

    QuadBatch quads_;
    SDL_GPUTexture* offscreen_ = nullptr;
    SDL_GPUTransferBuffer* readback_ = nullptr;
    Uint32 offscreen_w_ = 0;
    Uint32 offscreen_h_ = 0;

    RenderStats stats_;
    std::deque<float> frame_ms_;
    std::string pending_screenshot_;

    std::vector<QuadInstance> overlay_quads_;
};

} // namespace tb::render
