#pragma once

#include "render/renderer.h"
#include "sim/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <vector>

// The SDF primitive pipeline (06-rendering.md §8): instanced quads whose
// fragment stage evaluates analytic signed-distance fields. One draw call
// per (layer, blend) bucket.
namespace tb::render {

// 128 bytes, matching sdf.vert.hlsl's eight float4 instance rows.
struct SdfInstance {
    float cx, cy, hx, hy; // center + padded half-extents
    float rot;            // radians CCW
    uint32_t kind;        // 0 circle,1 ring,2 rbox,3 capsule,4 arc,5 ball
    float p0, p1;         // shape params (per-kind table, §8.1)
    float p2, p3, p4;
    float glow_radius;
    float fill0[4]; // premultiplied linear rgba, stop 0
    float fill1[4];
    float grad[4];   // xy dir, z len|radius, w mode 0/1/2
    float stroke[4]; // rgb premult linear, w width m (0 none)
    float glow[4];   // rgb linear, a intensity [0,2]
};

enum SdfKind : uint32_t {
    kSdfCircle = 0,
    kSdfRing = 1,
    kSdfRbox = 2,
    kSdfCapsule = 3,
    kSdfArc = 4,
    kSdfBall = 5,
};

class SdfBatch {
public:
    SdfBatch() = default;
    ~SdfBatch();
    SdfBatch(const SdfBatch&) = delete;
    SdfBatch& operator=(const SdfBatch&) = delete;

    bool init(SDL_GPUDevice* device,
              SDL_GPUTextureFormat target_format,
              const std::filesystem::path& shader_dir);
    void shutdown();

    void begin_frame(SDL_GPUCommandBuffer* cmd,
                     const float to_clip[16],
                     uint32_t width,
                     uint32_t height,
                     double time_s);
    void push(const SdfInstance& inst);
    void reserve(size_t count);
    void upload_and_draw(SDL_GPURenderPass* pass);

private:
    bool ensure_capacity(uint32_t count);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vert_ = nullptr;
    SDL_GPUShader* frag_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* quad_vertices_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUBuffer* gpu_ = nullptr;
    uint32_t capacity_ = 0;

    std::vector<SdfInstance> pending_;
    float to_clip_[16]{};
    SDL_GPUCommandBuffer* cmd_ = nullptr;
};

} // namespace tb::render
