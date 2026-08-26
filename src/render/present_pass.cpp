#include "render/present_pass.h"

#include "core/log.h"
#include "render/shader_load.h"
#include "render/view.h"

namespace tb::render {

namespace {

SDL_GPUShader*
load(SDL_GPUDevice* dev, ShaderStage stage, const std::filesystem::path& dir, const char* name) {
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
        ci.num_storage_buffers = 0;
        ci.num_storage_textures = 0;
        ci.num_uniform_buffers = stage == ShaderStage::Vertex ? 1 : 0;
        if (SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &ci)) {
            return sh;
        }
    }
    return nullptr;
}

} // namespace

PresentPass::~PresentPass() {
    shutdown();
}

bool PresentPass::init(SDL_GPUDevice* device,
                       SDL_GPUTextureFormat swap_format,
                       const std::filesystem::path& shader_dir) {
    device_ = device;
    vert_ = load(device_, ShaderStage::Vertex, shader_dir, "present.vert");
    frag_ = load(device_, ShaderStage::Fragment, shader_dir, "present.frag");
    if (!vert_ || !frag_) {
        TB_LOG_ERROR("main", "present pipeline: missing blobs in {}", shader_dir.string());
        shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &si);

    // Vertex layout: slot 0 interleaved pos(ndc) + uv.
    SDL_GPUVertexAttribute attrs[2]{};
    attrs[0].buffer_slot = 0;
    attrs[0].location = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = 0;
    attrs[1].buffer_slot = 0;
    attrs[1].location = 1;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset = sizeof(float) * 2;

    SDL_GPUVertexBufferDescription buf{};
    buf.slot = 0;
    buf.pitch = sizeof(CornerVertex);
    buf.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = &buf;
    vis.num_vertex_buffers = 1;
    vis.vertex_attributes = attrs;
    vis.num_vertex_attributes = 2;

    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vert_;
    pi.fragment_shader = frag_;
    pi.vertex_input_state = vis;
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
    ds.enable_depth_test = false;
    ds.enable_depth_write = false;
    pi.depth_stencil_state = ds;

    pi.target_info.num_color_targets = 1;
    SDL_GPUColorTargetDescription tgt{};
    tgt.format = swap_format; // no blending: opaque overwrite
    pi.target_info.color_target_descriptions = &tgt;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pi);
    if (!pipeline_) {
        TB_LOG_ERROR("main", "present pipeline create failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = 4 * sizeof(CornerVertex);
    upload_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = 4 * sizeof(CornerVertex);
    vertices_ = SDL_CreateGPUBuffer(device_, &bi);
    if (!sampler_ || !vertices_ || !upload_) {
        TB_LOG_ERROR("main", "present pass resource creation failed: {}", SDL_GetError());
        shutdown();
        return false;
    }
    return true;
}

void PresentPass::shutdown() {
    if (!device_) {
        return;
    }
    SDL_WaitForGPUIdle(device_);
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (vertices_) {
        SDL_ReleaseGPUBuffer(device_, vertices_);
        vertices_ = nullptr;
    }
    if (upload_) {
        SDL_ReleaseGPUTransferBuffer(device_, upload_);
        upload_ = nullptr;
    }
    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
    }
    if (vert_) {
        SDL_ReleaseGPUShader(device_, vert_);
        vert_ = nullptr;
    }
    if (frag_) {
        SDL_ReleaseGPUShader(device_, frag_);
        frag_ = nullptr;
    }
    device_ = nullptr;
}

void PresentPass::build_corners(const ViewTransform& view, uint32_t swap_w, uint32_t swap_h) {
    if (!device_ || !upload_) {
        return;
    }

    const float lw = float(view.rotation == 90u || view.rotation == 270u ? swap_h : swap_w);
    const float lh = float(view.rotation == 90u || view.rotation == 270u ? swap_w : swap_h);

    const float drawn_w = float(view.scene_w);
    const float drawn_h = float(view.scene_h);
    const float ox = (lw - drawn_w) * 0.5f;
    const float oy = (lh - drawn_h) * 0.5f;

    // Scene-quad corners in logical pixels → logical NDC → rotated
    // swapchain NDC (§6.4). UVs keep top-left origin.
    struct P {
        float x, y, u, v;
    };

    const P src[4] = {{ox, oy, 0.0f, 0.0f},
                      {ox + drawn_w, oy, 1.0f, 0.0f},
                      {ox, oy + drawn_h, 0.0f, 1.0f},
                      {ox + drawn_w, oy + drawn_h, 1.0f, 1.0f}};

    CornerVertex out[4];
    for (int i = 0; i < 4; ++i) {
        const float nx = src[i].x / lw * 2.0f - 1.0f;
        const float ny = 1.0f - src[i].y / lh * 2.0f; // y flip to NDC-up
        float rx, ry;
        rotate_ndc(view.rotation, nx, ny, rx, ry);
        out[i] = {{rx, ry}, {src[i].u, src[i].v}};
    }

    void* map = SDL_MapGPUTransferBuffer(device_, upload_, false);
    if (!map) {
        return;
    }
    SDL_memcpy(map, out, sizeof(out));
    SDL_UnmapGPUTransferBuffer(device_, upload_);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation s{upload_, 0};
    SDL_GPUBufferRegion d{vertices_, 0, 4 * sizeof(CornerVertex)};
    SDL_UploadToGPUBuffer(copy, &s, &d, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void PresentPass::add_pass(SDL_GPUCommandBuffer* cmd,
                           SDL_GPUTexture* scene,
                           SDL_GPUTexture* target) {
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    SDL_PushGPUVertexUniformData(cmd, 0, identity, sizeof(identity));

    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = target;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    tgt.clear_color = {0, 0, 0, 1}; // letterbox bars: opaque black

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUTextureSamplerBinding binding{scene, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_GPUBufferBinding vb{vertices_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace tb::render
