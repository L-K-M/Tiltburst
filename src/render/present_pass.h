#pragma once

#include "render/renderer.h"
#include "render/sdf_batch.h"
#include "render/view.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <vector>

// Present pass (06-rendering.md §12.5 pass F, bloomless until M13):
// samples the portrait scene target and draws it as the rotated letterbox
// quad with the exact piecewise sRGB encode.
namespace tb::render {

class PresentPass {
public:
    PresentPass() = default;
    ~PresentPass();
    PresentPass(const PresentPass&) = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    bool init(SDL_GPUDevice* device,
              SDL_GPUTextureFormat swap_format,
              const std::filesystem::path& shader_dir);
    void shutdown();

    // Builds the four letterbox-rect corners (swapchain NDC) and their
    // scene uvs for the current rotation and scene/swapchain sizes.
    void build_corners(const ViewTransform& view, uint32_t swap_w, uint32_t swap_h);

    void add_pass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene, SDL_GPUTexture* target);

private:
    struct CornerVertex {
        float pos[2];
        float uv[2];
    };

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vert_ = nullptr;
    SDL_GPUShader* frag_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* vertices_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
};

} // namespace tb::render
