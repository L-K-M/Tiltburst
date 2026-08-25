#pragma once

#include "render/renderer.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <vector>

// The M1 quad pipeline (06-rendering.md §9 shape, untextured flat tint).
// One instanced draw per frame for all overlay/debug quads.
namespace tb::render {

class QuadBatch {
public:
    QuadBatch() = default;
    ~QuadBatch();
    QuadBatch(const QuadBatch&) = delete;
    QuadBatch& operator=(const QuadBatch&) = delete;

    // Builds the sprite.vert/sprite.frag pipelines from the compiled blobs
    // in `shader_dir`, targeting `target_format`.
    bool init(SDL_GPUDevice* device,
              SDL_GPUTextureFormat target_format,
              const std::filesystem::path& shader_dir);
    void shutdown();

    // Per-frame state: pixel-space ortho over width x height with the
    // UI-space y flip (06 §6.4), plus sim time for u_time.
    void begin_frame(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, double time_s);
    void push(float cx, float cy, float hx, float hy, float r, float g, float b, float a);
    void reserve(size_t count);

    // Uploads queued instances (cycle=true) and draws them inside `pass`;
    // must be called on the frame's command buffer after begin_frame.
    // Clears the queue.
    void upload_and_draw(SDL_GPURenderPass* pass);

private:
    struct ViewPush {
        float m[16];
        float target_px[2];
        float ppm;
        float time;
    };

    bool ensure_instance_capacity(uint32_t count);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vert_ = nullptr;
    SDL_GPUShader* frag_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* quad_vertices_ = nullptr; // unit quad strip, static
    SDL_GPUTransferBuffer* instances_upload_ = nullptr;
    SDL_GPUBuffer* instances_gpu_ = nullptr;
    uint32_t instance_capacity_ = 0;

    std::vector<QuadInstance> pending_;
    ViewPush push_{};
    SDL_GPUCommandBuffer* current_cmd_ = nullptr;
};

} // namespace tb::render
