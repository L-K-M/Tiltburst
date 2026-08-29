#include "render/bloom_chain.h"

#include "core/log.h"
#include "render/shader_load.h"

namespace tb::render {

namespace {

SDL_GPUShader* load_shader(SDL_GPUDevice* dev,
                           ShaderStage stage,
                           const std::filesystem::path& dir,
                           const char* name,
                           uint32_t frag_uniforms) {
    struct Fmt {
        const char* ext;
        SDL_GPUShaderFormat format;
    };

    const Fmt fmts[] = {{".spv", SDL_GPU_SHADERFORMAT_SPIRV},
                        {".msl", SDL_GPU_SHADERFORMAT_MSL},
                        {".dxil", SDL_GPU_SHADERFORMAT_DXIL}};
    for (const Fmt& f : fmts) {
        auto code = load_shader_blob(dir / (std::string(name) + f.ext));
        if (code.empty()) {
            continue;
        }
        SDL_GPUShaderCreateInfo ci{};
        ci.code = code.data();
        ci.code_size = code.size();
        ci.format = f.format;
        ci.stage = stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX
                                                : SDL_GPU_SHADERSTAGE_FRAGMENT;
        ci.entrypoint = "main";
        ci.num_samplers = stage == ShaderStage::Fragment ? 1 : 0;
        ci.num_uniform_buffers = stage == ShaderStage::Vertex ? 0 : frag_uniforms;
        if (SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &ci)) {
            return sh;
        }
    }
    return nullptr;
}

} // namespace

BloomChain::~BloomChain() {
    shutdown();
}

SDL_GPUGraphicsPipeline*
BloomChain::make_pipe(SDL_GPUShader* frag, SDL_GPUTextureFormat format, bool additive) {
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vert_;
    pi.fragment_shader = frag;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;

    SDL_GPURasterizerState rs{};
    rs.fill_mode = SDL_GPU_FILLMODE_FILL;
    rs.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state = rs;

    SDL_GPUMultisampleState ms{};
    ms.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pi.multisample_state = ms;

    SDL_GPUDepthStencilState ds{};
    ds.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    pi.depth_stencil_state = ds;

    pi.target_info.num_color_targets = 1;
    SDL_GPUColorTargetDescription tgt{};
    tgt.format = format;
    if (additive) {
        // §12.4: bilinear upsample drawn ADDITIVELY into the wider
        // level (premultiplied src, one/one blend).
        tgt.blend_state.enable_blend = true;
        tgt.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        tgt.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        tgt.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        tgt.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        tgt.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        tgt.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    pi.target_info.color_target_descriptions = &tgt;
    return SDL_CreateGPUGraphicsPipeline(device_, &pi);
}

bool BloomChain::init(SDL_GPUDevice* device, const std::filesystem::path& shader_dir) {
    device_ = device;
    vert_ = load_shader(device_, ShaderStage::Vertex, shader_dir, "bloom.vert", 0);
    bright_frag_ = load_shader(device_, ShaderStage::Fragment, shader_dir, "bloom_bright.frag", 0);
    down_frag_ = load_shader(device_, ShaderStage::Fragment, shader_dir, "bloom_down.frag", 1);
    blur_frag_ = load_shader(device_, ShaderStage::Fragment, shader_dir, "bloom_blur.frag", 1);
    up_frag_ = load_shader(device_, ShaderStage::Fragment, shader_dir, "bloom_up.frag", 0);
    if (!vert_ || !bright_frag_ || !down_frag_ || !blur_frag_ || !up_frag_) {
        TB_LOG_WARN("render", "bloom: missing blobs in {}", shader_dir.string());
        shutdown();
        return false;
    }

    // All bloom targets are RGBA16F (ensure_targets); the pipelines
    // must match THAT format, not the scene format (cycle-1 blocker).
    bright_pipe_ = make_pipe(bright_frag_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, false);
    down_pipe_ = make_pipe(down_frag_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, false);
    blur_pipe_ = make_pipe(blur_frag_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, false);
    up_pipe_ = make_pipe(up_frag_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, true);
    if (!bright_pipe_ || !down_pipe_ || !blur_pipe_ || !up_pipe_) {
        TB_LOG_ERROR("render", "bloom pipeline creation failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &si);
    if (sampler_ == nullptr) {
        TB_LOG_ERROR("render", "bloom sampler creation failed: {}", SDL_GetError());
        shutdown(); // release the already-created shaders + pipelines
        return false;
    }
    return true;
}

void BloomChain::destroy_targets() {
    for (Level& lv : levels_) {
        if (lv.tex != nullptr) {
            SDL_ReleaseGPUTexture(device_, lv.tex);
            lv.tex = nullptr;
        }
        if (lv.blur != nullptr) {
            SDL_ReleaseGPUTexture(device_, lv.blur);
            lv.blur = nullptr;
        }
        lv.w = lv.h = 0;
    }
}

void BloomChain::shutdown() {
    if (!device_) {
        return;
    }
    SDL_WaitForGPUIdle(device_);
    destroy_targets();
    for (SDL_GPUGraphicsPipeline** pipe : {&bright_pipe_, &down_pipe_, &blur_pipe_, &up_pipe_}) {
        if (*pipe) {
            SDL_ReleaseGPUGraphicsPipeline(device_, *pipe);
            *pipe = nullptr;
        }
    }
    for (SDL_GPUShader** sh : {&vert_, &bright_frag_, &down_frag_, &blur_frag_, &up_frag_}) {
        if (*sh) {
            SDL_ReleaseGPUShader(device_, *sh);
            *sh = nullptr;
        }
    }
    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
    }
    device_ = nullptr;
}

bool BloomChain::ensure_targets(uint32_t scene_w, uint32_t scene_h) {
    if (!device_) {
        return false;
    }
    // Levels at 1/2, 1/4, 1/8 (§12.2).
    bool changed = false;
    uint32_t w = scene_w;
    uint32_t h = scene_h;
    for (Level& lv : levels_) {
        w = std::max(w / 2u, 1u);
        h = std::max(h / 2u, 1u);
        // BOTH targets must exist at the right size — a partial
        // allocation (tex ok, blur null) must retry, not skip
        // (cycle-12 review).
        if (lv.tex != nullptr && lv.blur != nullptr && lv.w == w && lv.h == h) {
            continue;
        }
        if (lv.tex != nullptr) {
            SDL_ReleaseGPUTexture(device_, lv.tex);
            lv.tex = nullptr;
        }
        if (lv.blur != nullptr) {
            SDL_ReleaseGPUTexture(device_, lv.blur);
            lv.blur = nullptr;
        }
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        lv.tex = SDL_CreateGPUTexture(device_, &ti);
        lv.blur = SDL_CreateGPUTexture(device_, &ti);
        lv.w = w;
        lv.h = h;
        changed = true;
        if (lv.tex == nullptr || lv.blur == nullptr) {
            TB_LOG_ERROR("render", "bloom target creation failed: {}", SDL_GetError());
            return false;
        }
    }
    // No WaitGPUIdle: SDL_gpu defers released-texture destruction
    // past in-flight command buffers, so a resize doesn't need a full
    // device stall.
    (void)changed;
    return true;
}

void BloomChain::draw_fullscreen(SDL_GPURenderPass* pass) {
    // The vertex shader generates the quad from SV_VertexID — no
    // vertex buffer binding.
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}

void BloomChain::record(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* scene) {
    if (!device_ || quality_ == Quality::Off) {
        return;
    }
    for (const Level& lv : levels_) {
        if (lv.tex == nullptr || lv.blur == nullptr) {
            return; // ensure_targets failed: skip the chain entirely
        }
    }

    // ---- §12.1 bright pass: scene → level0 ----
    {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture = levels_[0].tex;
        tgt.load_op = SDL_GPU_LOADOP_DONT_CARE;
        tgt.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, bright_pipe_);
        SDL_GPUTextureSamplerBinding b{scene, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
        draw_fullscreen(pass);
        SDL_EndGPURenderPass(pass);
    }

    // ---- §12.2 downsample: levelN-1 → levelN (2 passes) ----
    for (int lv = 1; lv < 3; ++lv) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture = levels_[lv].tex;
        tgt.load_op = SDL_GPU_LOADOP_DONT_CARE;
        tgt.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, down_pipe_);
        const float texel[2] = {1.0f / float(levels_[lv - 1].w), 1.0f / float(levels_[lv - 1].h)};
        SDL_PushGPUFragmentUniformData(cmd, 0, texel, sizeof(texel));
        SDL_GPUTextureSamplerBinding b{levels_[lv - 1].tex, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
        draw_fullscreen(pass);
        SDL_EndGPURenderPass(pass);
    }

    // ---- §12.3 blur: per level, horizontal into .blur, vertical back
    // (6 passes) ----
    for (int lv = 0; lv < 3; ++lv) {
        const float texel[2] = {1.0f / float(levels_[lv].w), 1.0f / float(levels_[lv].h)};
        for (int dir = 0; dir < 2; ++dir) {
            SDL_GPUTexture* src = dir == 0 ? levels_[lv].tex : levels_[lv].blur;
            SDL_GPUTexture* dst = dir == 0 ? levels_[lv].blur : levels_[lv].tex;
            SDL_GPUColorTargetInfo tgt{};
            tgt.texture = dst;
            tgt.load_op = SDL_GPU_LOADOP_DONT_CARE;
            tgt.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, blur_pipe_);
            // u_dir = texel step * axis (§12.3).
            const float u_dir[2] = {dir == 0 ? texel[0] : 0.0f, dir == 0 ? 0.0f : texel[1]};
            SDL_PushGPUFragmentUniformData(cmd, 0, u_dir, sizeof(u_dir));
            SDL_GPUTextureSamplerBinding b{src, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
            draw_fullscreen(pass);
            SDL_EndGPURenderPass(pass);
        }
    }

    // ---- §12.4 upsample+additive: bloom2 → bloom1 → bloom0 (2 passes)
    // ----
    for (int lv = 2; lv >= 1; --lv) {
        SDL_GPUColorTargetInfo tgt{};
        tgt.texture = levels_[lv - 1].tex;
        tgt.load_op = SDL_GPU_LOADOP_LOAD; // additive into the wider level
        tgt.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, up_pipe_);
        SDL_GPUTextureSamplerBinding b{levels_[lv].tex, sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
        draw_fullscreen(pass);
        SDL_EndGPURenderPass(pass);
    }
}

} // namespace tb::render
