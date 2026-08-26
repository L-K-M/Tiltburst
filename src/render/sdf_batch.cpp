#include "render/sdf_batch.h"

#include "core/log.h"
#include "render/shader_load.h"

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
        ci.num_samplers = 0;
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

SdfBatch::~SdfBatch() {
    shutdown();
}

bool SdfBatch::init(SDL_GPUDevice* device,
                    SDL_GPUTextureFormat target_format,
                    const std::filesystem::path& shader_dir) {
    device_ = device;
    vert_ = load(device_, ShaderStage::Vertex, shader_dir, "sdf.vert");
    frag_ = load(device_, ShaderStage::Fragment, shader_dir, "sdf.frag");
    if (!vert_ || !frag_) {
        TB_LOG_ERROR("main", "sdf pipeline: missing sdf blobs in {}", shader_dir.string());
        shutdown();
        return false;
    }

    const float strip[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    SDL_GPUTransferBufferCreateInfo up{};
    up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    up.size = sizeof(strip);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &up);
    if (!tb) {
        shutdown();
        return false;
    }
    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        shutdown();
        return false;
    }
    SDL_memcpy(map, strip, sizeof(strip));
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = sizeof(strip);
    quad_vertices_ = SDL_CreateGPUBuffer(device_, &bi);
    bool ok = quad_vertices_ != nullptr;
    if (ok) {
        SDL_GPUTransferBufferLocation src{tb, 0};
        SDL_GPUBufferRegion dst{quad_vertices_, 0, sizeof(strip)};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    if (!ok) {
        shutdown();
        return false;
    }

    // Slot 0: corner float2. Slot 1: SdfInstance (eight float4 rows).
    SDL_GPUVertexAttribute attrs[9]{};
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0};
    for (uint32_t i = 1; i < 9; ++i) {
        attrs[i].buffer_slot = 1;
        attrs[i].location = i;
        attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[i].offset = sizeof(float) * 4 * (i - 1);
    }

    SDL_GPUVertexBufferDescription bufs[2]{};
    bufs[0].slot = 0;
    bufs[0].pitch = sizeof(float) * 2;
    bufs[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    bufs[1].slot = 1;
    bufs[1].pitch = sizeof(SdfInstance);
    bufs[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    SDL_GPUVertexInputState vis{};
    vis.vertex_buffer_descriptions = bufs;
    vis.num_vertex_buffers = 2;
    vis.vertex_attributes = attrs;
    vis.num_vertex_attributes = 9;

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

    // Premultiplied alpha (§7).
    SDL_GPUColorTargetBlendState blend{};
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.enable_blend = true;
    blend.color_write_mask = 0xF;

    SDL_GPUColorTargetDescription tgt{};
    tgt.format = target_format;
    tgt.blend_state = blend;
    pi.target_info.num_color_targets = 1;
    pi.target_info.color_target_descriptions = &tgt;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pi);
    if (!pipeline_) {
        TB_LOG_ERROR("main", "sdf SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    if (!ensure_capacity(512)) {
        TB_LOG_ERROR("main", "sdf instance buffer allocation failed: {}", SDL_GetError());
        shutdown();
        return false;
    }
    return true;
}

void SdfBatch::shutdown() {
    if (!device_) {
        return;
    }
    SDL_WaitForGPUIdle(device_);
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (gpu_) {
        SDL_ReleaseGPUBuffer(device_, gpu_);
        gpu_ = nullptr;
    }
    if (upload_) {
        SDL_ReleaseGPUTransferBuffer(device_, upload_);
        upload_ = nullptr;
    }
    if (quad_vertices_) {
        SDL_ReleaseGPUBuffer(device_, quad_vertices_);
        quad_vertices_ = nullptr;
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

void SdfBatch::begin_frame(SDL_GPUCommandBuffer* cmd,
                           const float to_clip[16],
                           uint32_t width,
                           uint32_t height,
                           double time_s) {
    cmd_ = cmd;
    for (int i = 0; i < 16; ++i) {
        to_clip_[i] = to_clip[i];
    }
    (void)width;
    (void)height;
    (void)time_s;
    pending_.clear();
}

void SdfBatch::push(const SdfInstance& inst) {
    pending_.push_back(inst);
}

bool SdfBatch::ensure_capacity(uint32_t count) {
    if (count <= capacity_ && gpu_ != nullptr && upload_ != nullptr) {
        return true;
    }
    uint32_t cap = capacity_ == 0 ? 512 : capacity_;
    while (cap < count) {
        cap *= 2;
    }
    capacity_ = cap;

    if (gpu_) {
        SDL_ReleaseGPUBuffer(device_, gpu_);
        gpu_ = nullptr;
    }
    if (upload_) {
        SDL_ReleaseGPUTransferBuffer(device_, upload_);
        upload_ = nullptr;
    }

    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = capacity_ * sizeof(SdfInstance);
    gpu_ = SDL_CreateGPUBuffer(device_, &bi);

    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = capacity_ * sizeof(SdfInstance);
    upload_ = SDL_CreateGPUTransferBuffer(device_, &ti);

    return gpu_ && upload_;
}

void SdfBatch::reserve(size_t count) {
    ensure_capacity(uint32_t(count));
}

void SdfBatch::upload_and_draw(SDL_GPURenderPass* pass) {
    if (pending_.empty() || !cmd_) {
        return;
    }
    if (!ensure_capacity(uint32_t(pending_.size()))) {
        pending_.clear();
        return;
    }

    struct ViewPush {
        float m[16];
        float target_px[2];
        float ppm;
        float time;
    } push{};

    for (int i = 0; i < 16; ++i) {
        push.m[i] = to_clip_[i];
    }
    SDL_PushGPUVertexUniformData(cmd_, 0, &push, sizeof(push));

    void* map = SDL_MapGPUTransferBuffer(device_, upload_, true);
    if (!map) {
        pending_.clear();
        return;
    }
    SDL_memcpy(map, pending_.data(), pending_.size() * sizeof(SdfInstance));
    SDL_UnmapGPUTransferBuffer(device_, upload_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
    SDL_GPUTransferBufferLocation src{upload_, 0};
    SDL_GPUBufferRegion dst{gpu_, 0, uint32_t(pending_.size() * sizeof(SdfInstance))};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true); // cycle: per-frame
    SDL_EndGPUCopyPass(copy);

    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBufferBinding bindings[2]{};
    bindings[0].buffer = quad_vertices_;
    bindings[0].offset = 0;
    bindings[1].buffer = gpu_;
    bindings[1].offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, bindings, 2);
    SDL_DrawGPUPrimitives(pass, 4, uint32_t(pending_.size()), 0, 0);

    pending_.clear();
}

} // namespace tb::render
