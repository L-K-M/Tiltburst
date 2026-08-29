#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>

// The bloom chain (06-rendering.md §12.1–§12.4): bright pass → 2×
// downsample → 3 levels × separable blur (H+V) → 2 upsample+additive.
// 11 post passes total, all at ≤ 1/2 res. The final bloom0 texture
// feeds the composite (§12.5).
namespace tb::render {

class BloomChain {
public:
    // Quality: low skips levels entirely (§12 — the disabled fallback
    // renders with the plain composite).
    enum class Quality : uint8_t { Off = 0, On = 1 };

    BloomChain() = default;
    ~BloomChain();
    BloomChain(const BloomChain&) = delete;
    BloomChain& operator=(const BloomChain&) = delete;

    bool init(SDL_GPUDevice* device,
              SDL_GPUTextureFormat scene_format,
              const std::filesystem::path& shader_dir);
    void shutdown();

    // (Re)creates the level targets for a scene size change. Must be
    // called once before the first run.
    bool ensure_targets(uint32_t scene_w, uint32_t scene_h);

    // Records the 11 passes into `cmd` reading `scene`. The combined
    // result is bloom0(); pass that to the composite.
    void record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene);

    // The combined 3-level bloom (§12.4); the composite samples this.
    SDL_GPUTexture* bloom0() const { return levels_[0].tex; }

    void set_quality(Quality q) { quality_ = q; }

    Quality quality() const { return quality_; }

private:
    struct Level {
        SDL_GPUTexture* tex = nullptr;  // the level's working target
        SDL_GPUTexture* blur = nullptr; // horizontal-pass scratch
        uint32_t w = 0, h = 0;
    };

    SDL_GPUDevice* device_ = nullptr;
    Level levels_[3]; // 1/2, 1/4, 1/8 res
    Quality quality_ = Quality::On;

    // Pipelines (shared vertex).
    SDL_GPUShader* vert_ = nullptr;
    SDL_GPUShader* bright_frag_ = nullptr;
    SDL_GPUShader* down_frag_ = nullptr;
    SDL_GPUShader* blur_frag_ = nullptr;
    SDL_GPUShader* up_frag_ = nullptr;
    SDL_GPUGraphicsPipeline* bright_pipe_ = nullptr;
    SDL_GPUGraphicsPipeline* down_pipe_ = nullptr;
    SDL_GPUGraphicsPipeline* blur_pipe_ = nullptr;
    SDL_GPUGraphicsPipeline* up_pipe_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;

    void destroy_targets();
    SDL_GPUGraphicsPipeline*
    make_pipe(SDL_GPUShader* frag, SDL_GPUTextureFormat format, bool additive);
    void draw_fullscreen(SDL_GPURenderPass* pass);
};

} // namespace tb::render
