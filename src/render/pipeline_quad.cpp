#include "render/pipeline_quad.h"

#include "core/log.h"
#include "render/shader_load.h"

namespace tb::render {

namespace {

// UI pixel space -> clip with the top-left-origin y flip (06 §6.4):
// x_ndc = 2*px/w - 1; y_ndc = 1 - 2*py/h. Column-major for
// HLSL mul(row-vector, matrix).
void ui_to_clip(float width, float height, float out[16]) {
    const float sx = 2.0f / width;
    const float sy = -2.0f / height;
    out[0] = sx;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;

    out[4] = 0.0f;
    out[5] = sy;
    out[6] = 0.0f;
    out[7] = 0.0f;

    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 1.0f;
    out[11] = 0.0f;

    out[12] = -1.0f;
    out[13] = 1.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

SDL_GPUShader* load_shader(SDL_GPUDevice* device,
                           ShaderStage stage,
                           const std::filesystem::path& dir,
                           const char* name) {
    struct FormatInfo {
        const char* ext;
        SDL_GPUShaderFormat format;
    };

    const FormatInfo candidates[] = {
        {".spv", SDL_GPU_SHADERFORMAT_SPIRV},
        {".msl", SDL_GPU_SHADERFORMAT_MSL},
        {".dxil", SDL_GPU_SHADERFORMAT_DXIL},
    };

    const SDL_GPUShaderStage sdl_stage =
        stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;

    for (const FormatInfo& info : candidates) {
        std::vector<uint8_t> code = load_shader_blob(dir / (std::string(name) + info.ext));
        if (code.empty()) {
            continue;
        }

        SDL_GPUShaderCreateInfo ci{};
        ci.code = code.data();
        ci.code_size = code.size();
        ci.format = info.format;
        ci.stage = sdl_stage;
        ci.entrypoint = "main";
        // M1 quad pipeline: no samplers/textures; one vertex uniform
        // (ViewUniforms at b0, space1), zero fragment uniforms.
        ci.num_samplers = 0;
        ci.num_storage_buffers = 0;
        ci.num_storage_textures = 0;
        ci.num_uniform_buffers = stage == ShaderStage::Vertex ? 1 : 0;

        if (SDL_GPUShader* shader = SDL_CreateGPUShader(device, &ci)) {
            return shader;
        }
        TB_LOG_WARN("main", "SDL_CreateGPUShader({}{}) failed: {}", name, info.ext, SDL_GetError());
    }
    return nullptr;
}

} // namespace

QuadBatch::~QuadBatch() {
    shutdown();
}

bool QuadBatch::init(SDL_GPUDevice* device,
                     SDL_GPUTextureFormat target_format,
                     const std::filesystem::path& shader_dir) {
    device_ = device;

    vert_ = load_shader(device_, ShaderStage::Vertex, shader_dir, "sprite.vert");
    frag_ = load_shader(device_, ShaderStage::Fragment, shader_dir, "sprite.frag");
    if (!vert_ || !frag_) {
        TB_LOG_ERROR("main", "quad pipeline: missing sprite blobs in {}", shader_dir.string());
        shutdown();
        return false;
    }

    // Static unit-quad strip: corner in {-1,+1}^2, 4 vertices.
    const float quad_strip[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    SDL_GPUTransferBufferCreateInfo quad_up{};
    quad_up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    quad_up.size = sizeof(quad_strip);
    SDL_GPUTransferBuffer* quad_upload = SDL_CreateGPUTransferBuffer(device_, &quad_up);
    if (!quad_upload) {
        shutdown();
        return false;
    }
    void* map = SDL_MapGPUTransferBuffer(device_, quad_upload, false);
    SDL_memcpy(map, quad_strip, sizeof(quad_strip));
    SDL_UnmapGPUTransferBuffer(device_, quad_upload);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    binfo.size = sizeof(quad_strip);
    quad_vertices_ = SDL_CreateGPUBuffer(device_, &binfo);
    bool ok = quad_vertices_ != nullptr;
    if (ok) {
        SDL_GPUTransferBufferLocation src{quad_upload, 0};
        SDL_GPUBufferRegion dst{quad_vertices_, 0, sizeof(quad_strip)};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, quad_upload);
    if (!ok) {
        shutdown();
        return false;
    }

    // Vertex layout: slot 0 = corner (per-vertex), slot 1 = instance data.
    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0].buffer_slot = 0;
    attrs[0].location = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = 0;
    attrs[1].buffer_slot = 1;
    attrs[1].location = 1;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[1].offset = 0;
    attrs[2].buffer_slot = 1;
    attrs[2].location = 2;
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[2].offset = sizeof(float) * 4;

    SDL_GPUVertexBufferDescription buffers[2]{};
    buffers[0].slot = 0;
    buffers[0].pitch = sizeof(float) * 2;
    buffers[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    buffers[1].slot = 1;
    buffers[1].pitch = sizeof(QuadInstance);
    buffers[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;

    SDL_GPUVertexInputState input_state{};
    input_state.vertex_buffer_descriptions = buffers;
    input_state.num_vertex_buffers = 2;
    input_state.vertex_attributes = attrs;
    input_state.num_vertex_attributes = 3;

    SDL_GPUGraphicsPipelineCreateInfo pinfo{};
    pinfo.vertex_shader = vert_;
    pinfo.fragment_shader = frag_;
    pinfo.vertex_input_state = input_state;
    pinfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;

    SDL_GPURasterizerState rasterizer{};
    rasterizer.fill_mode = SDL_GPU_FILLMODE_FILL;
    rasterizer.cull_mode = SDL_GPU_CULLMODE_NONE;
    pinfo.rasterizer_state = rasterizer;

    SDL_GPUMultisampleState msaa{};
    msaa.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pinfo.multisample_state = msaa;

    SDL_GPUDepthStencilState depth{};
    depth.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    depth.enable_depth_test = false;
    depth.enable_depth_write = false;
    pinfo.depth_stencil_state = depth;

    // Premultiplied alpha, exactly as §7: color/alpha ONE,
    // ONE_MINUS_SRC_ALPHA, ADD.
    SDL_GPUColorTargetBlendState blend{};
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.enable_blend = true;
    blend.color_write_mask = 0xF;

    SDL_GPUColorTargetDescription target{};
    target.format = target_format;
    target.blend_state = blend;
    pinfo.target_info.num_color_targets = 1;
    pinfo.target_info.color_target_descriptions = &target;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pinfo);
    if (!pipeline_) {
        TB_LOG_ERROR("main", "SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    reserve(1024);
    return true;
}

void QuadBatch::shutdown() {
    if (!device_) {
        return;
    }
    SDL_WaitForGPUIdle(device_);
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }
    if (instances_gpu_) {
        SDL_ReleaseGPUBuffer(device_, instances_gpu_);
        instances_gpu_ = nullptr;
    }
    if (instances_upload_) {
        SDL_ReleaseGPUTransferBuffer(device_, instances_upload_);
        instances_upload_ = nullptr;
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

void QuadBatch::begin_frame(SDL_GPUCommandBuffer* cmd,
                            uint32_t width,
                            uint32_t height,
                            double time_s) {
    current_cmd_ = cmd;
    ui_to_clip(float(width), float(height), push_.m);
    push_.target_px[0] = float(width);
    push_.target_px[1] = float(height);
    push_.ppm = 0.0f;
    push_.time = float(time_s);
    pending_.clear();
}

void QuadBatch::push(float cx, float cy, float hx, float hy, float r, float g, float b, float a) {
    QuadInstance q{cx, cy, hx, hy, r, g, b, a};
    pending_.push_back(q);
}

bool QuadBatch::ensure_instance_capacity(uint32_t count) {
    if (count <= instance_capacity_) {
        return true;
    }
    uint32_t new_cap = instance_capacity_ == 0 ? 1024 : instance_capacity_;
    while (new_cap < count) {
        new_cap *= 2;
    }
    instance_capacity_ = new_cap;

    if (instances_gpu_) {
        SDL_ReleaseGPUBuffer(device_, instances_gpu_);
        instances_gpu_ = nullptr;
    }
    if (instances_upload_) {
        SDL_ReleaseGPUTransferBuffer(device_, instances_upload_);
        instances_upload_ = nullptr;
    }

    SDL_GPUBufferCreateInfo binfo{};
    binfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    binfo.size = instance_capacity_ * sizeof(QuadInstance);
    instances_gpu_ = SDL_CreateGPUBuffer(device_, &binfo);

    SDL_GPUTransferBufferCreateInfo tinfo{};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = instance_capacity_ * sizeof(QuadInstance);
    instances_upload_ = SDL_CreateGPUTransferBuffer(device_, &tinfo);
    return instances_gpu_ && instances_upload_;
}

void QuadBatch::reserve(size_t count) {
    ensure_instance_capacity(uint32_t(count));
}

void QuadBatch::upload_and_draw(SDL_GPURenderPass* pass) {
    if (pending_.empty() || !current_cmd_) {
        return;
    }
    if (!ensure_instance_capacity(uint32_t(pending_.size()))) {
        pending_.clear();
        return;
    }

    SDL_PushGPUVertexUniformData(current_cmd_, 0, &push_, sizeof(push_));

    void* map = SDL_MapGPUTransferBuffer(device_, instances_upload_, true);
    SDL_memcpy(map, pending_.data(), pending_.size() * sizeof(QuadInstance));
    SDL_UnmapGPUTransferBuffer(device_, instances_upload_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(current_cmd_);
    SDL_GPUTransferBufferLocation src{instances_upload_, 0};
    SDL_GPUBufferRegion dst{instances_gpu_, 0, uint32_t(pending_.size() * sizeof(QuadInstance))};
    SDL_UploadToGPUBuffer(copy, &src, &dst,
                          true); // cycle: per-frame upload under FIF 1
    SDL_EndGPUCopyPass(copy);

    SDL_BindGPUGraphicsPipeline(pass, pipeline_);

    SDL_GPUBufferBinding bindings[2]{};
    bindings[0].buffer = quad_vertices_;
    bindings[0].offset = 0;
    bindings[1].buffer = instances_gpu_;
    bindings[1].offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, bindings, 2);

    SDL_DrawGPUPrimitives(pass, 4, uint32_t(pending_.size()), 0, 0);

    pending_.clear();
}

} // namespace tb::render
