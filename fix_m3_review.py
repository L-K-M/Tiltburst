import io

def patch(path, pairs):
    s = open(path, encoding="utf-8").read()
    for old, new in pairs:
        assert old in s, f"{path}: pattern missing:\n{old[:120]}"
        s = s.replace(old, new)
    open(path, "w", encoding="utf-8").write(s)
    print("patched", path)

# --- sdf.frag.hlsl
patch("shaders/sdf.frag.hlsl", [
("""struct FSIn
{
    float4 pos : SV_Position;
    float2 lp : TEXCOORD0;
    float4 n0 : TEXCOORD1; // rot, kind, p0, p1""",
"""struct FSIn
{
    float4 pos : SV_Position;
    float2 lp : TEXCOORD0;
    nointerpolation float4 n0 : TEXCOORD1; // rot, kind, p0, p1"""),
])

s = open("shaders/sdf.frag.hlsl", encoding="utf-8").read()
for i, name in [(1, "n0"), (2, "n1"), (3, "n2"), (4, "n3"), (5, "n4"), (6, "n5"), (6, "n6")]:
    pass
s = s.replace("float4 n1 : TEXCOORD2;", "nointerpolation float4 n1 : TEXCOORD2;")
s = s.replace("float4 n2 : TEXCOORD3;", "nointerpolation float4 n2 : TEXCOORD3;")
s = s.replace("float4 n3 : TEXCOORD4;", "nointerpolation float4 n3 : TEXCOORD4;")
s = s.replace("float4 n4 : TEXCOORD5;", "nointerpolation float4 n4 : TEXCOORD5;")
s = s.replace("float4 n5 : TEXCOORD6;", "nointerpolation float4 n5 : TEXCOORD6;")
s = s.replace("float4 n6 : TEXCOORD7;", "nointerpolation float4 n6 : TEXCOORD7;")
# gradient length guard
s = s.replace("""float4 eval_fill(float4 f0, float4 f1, float4 grad, float2 lp)
{
    if (grad.w == 0.0) return f0;
    float t = (grad.w == 1.0)
                  ? saturate(dot(lp, grad.xy) / grad.z + 0.5)
                  : saturate(length(lp) / grad.z);
    return lerp(f0, f1, t);
}""",
"""float4 eval_fill(float4 f0, float4 f1, float4 grad, float2 lp)
{
    if (grad.w == 0.0) return f0;
    const float glen = max(grad.z, 1e-6); // grad.z == 0 -> inf/NaN fill
    float t = (grad.w == 1.0)
                  ? saturate(dot(lp, grad.xy) / glen + 0.5)
                  : saturate(length(lp) / glen);
    return lerp(f0, f1, t);
}""")
open("shaders/sdf.frag.hlsl", "w", encoding="utf-8").write(s)

# --- sdf.vert.hlsl nointerpolation
p = "shaders/sdf.vert.hlsl"
s = open(p, encoding="utf-8").read()
s = s.replace("float4 n0 : TEXCOORD1;", "nointerpolation float4 n0 : TEXCOORD1;")
s = s.replace("float4 n1 : TEXCOORD2;", "nointerpolation float4 n1 : TEXCOORD2;")
s = s.replace("float4 n2 : TEXCOORD3;", "nointerpolation float4 n2 : TEXCOORD3;")
s = s.replace("float4 n3 : TEXCOORD4;", "nointerpolation float4 n3 : TEXCOORD4;")
s = s.replace("float4 n4 : TEXCOORD5;", "nointerpolation float4 n4 : TEXCOORD5;")
s = s.replace("float4 n5 : TEXCOORD6;", "nointerpolation float4 n5 : TEXCOORD6;")
s = s.replace("float4 n6 : TEXCOORD7;", "nointerpolation float4 n6 : TEXCOORD7;")
open(p, "w", encoding="utf-8").write(s)

# --- present.frag.hlsl clamp
p = "shaders/present.frag.hlsl"
s = open(p, encoding="utf-8").read()
s = s.replace("const float3 c = scene.Sample(samp, i.uv).rgb;",
              "const float3 c = max(scene.Sample(samp, i.uv).rgb, 0.0f); // pow(neg) -> NaN")
open(p, "w", encoding="utf-8").write(s)
print("shaders ok")

# --- present_pass.cpp: resource checks + map guard
p = "src/render/present_pass.cpp"
s = open(p, encoding="utf-8").read()
old = """    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pi);
    if (!pipeline_) {
        TB_LOG_ERROR("main", "present pipeline create failed: {}",
                     SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo ti {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = 4 * sizeof(CornerVertex);
    upload_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    SDL_GPUBufferCreateInfo bi {};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = 4 * sizeof(CornerVertex);
    vertices_ = SDL_CreateGPUBuffer(device_, &bi);
    return vertices_ && upload_;
}"""
new = """    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pi);
    if (!pipeline_) {
        TB_LOG_ERROR("main", "present pipeline create failed: {}",
                     SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo ti {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = 4 * sizeof(CornerVertex);
    upload_ = SDL_CreateGPUTransferBuffer(device_, &ti);
    SDL_GPUBufferCreateInfo bi {};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = 4 * sizeof(CornerVertex);
    vertices_ = SDL_CreateGPUBuffer(device_, &bi);
    if (!sampler_ || !vertices_ || !upload_) {
        TB_LOG_ERROR("main", "present pass resource creation failed: {}",
                     SDL_GetError());
        shutdown();
        return false;
    }
    return true;
}"""
assert old in s
s = s.replace(old, new)

old2 = """    void* map = SDL_MapGPUTransferBuffer(device_, upload_, false);
    SDL_memcpy(map, out, sizeof(out));
    SDL_UnmapGPUTransferBuffer(device_, upload_);"""
new2 = """    void* map = SDL_MapGPUTransferBuffer(device_, upload_, false);
    if (!map) {
        return;
    }
    SDL_memcpy(map, out, sizeof(out));
    SDL_UnmapGPUTransferBuffer(device_, upload_);"""
assert old2 in s
s = s.replace(old2, new2)
open(p, "w", encoding="utf-8").write(s)
print("present_pass ok")

# --- sdf_batch.h: static_assert + state members
p = "src/render/sdf_batch.h"
s = open(p, encoding="utf-8").read()
s = s.replace("""    float glow[4];               // rgb linear, a intensity [0,2]
};""",
"""    float glow[4];               // rgb linear, a intensity [0,2]
};
static_assert(sizeof(SdfInstance) == 128,
              "must match sdf.vert.hlsl's eight float4 rows");""")
s = s.replace("""    std::vector<SdfInstance> pending_;
    float to_clip_[16] {};
    SDL_GPUCommandBuffer* cmd_ = nullptr;""",
"""    std::vector<SdfInstance> pending_;
    float to_clip_[16] {};
    float target_px_[2] = {0.0f, 0.0f};
    float ppm_ = 0.0f;
    float time_ = 0.0f;
    SDL_GPUCommandBuffer* cmd_ = nullptr;""")
open(p, "w", encoding="utf-8").write(s)

# --- sdf_batch.cpp: begin_frame stores state; push populated; guards
p = "src/render/sdf_batch.cpp"
s = open(p, encoding="utf-8").read()
s = s.replace("""void SdfBatch::begin_frame(SDL_GPUCommandBuffer* cmd, const float to_clip[16],
                           uint32_t width, uint32_t height, double time_s) {
    cmd_ = cmd;
    for (int i = 0; i < 16; ++i) {
        to_clip_[i] = to_clip[i];
    }
    (void)width;
    (void)height;
    (void)time_s;
    pending_.clear();
}""",
"""void SdfBatch::begin_frame(SDL_GPUCommandBuffer* cmd, const float to_clip[16],
                           uint32_t width, uint32_t height, double time_s) {
    cmd_ = cmd;
    for (int i = 0; i < 16; ++i) {
        to_clip_[i] = to_clip[i];
    }
    target_px_[0] = static_cast<float>(width);
    target_px_[1] = static_cast<float>(height);
    ppm_ = 0.0f; // pixel-space passes carry ppm via uniforms later (M13)
    time_ = static_cast<float>(time_s);
    pending_.clear();
}""")
s = s.replace("""    } push {};
    for (int i = 0; i < 16; ++i) {
        push.m[i] = to_clip_[i];
    }
    SDL_PushGPUVertexUniformData(cmd_, 0, &push, sizeof(push));""",
"""    } push {};
    for (int i = 0; i < 16; ++i) {
        push.m[i] = to_clip_[i];
    }
    push.target_px[0] = target_px_[0];
    push.target_px[1] = target_px_[1];
    push.ppm = ppm_;
    push.time = time_;
    SDL_PushGPUVertexUniformData(cmd_, 0, &push, sizeof(push));""")
s = s.replace("""bool SdfBatch::ensure_capacity(uint32_t count) {
    if (count <= capacity_) {
        return true;
    }""",
"""bool SdfBatch::ensure_capacity(uint32_t count) {
    if (count <= capacity_ && gpu_ != nullptr && upload_ != nullptr) {
        return true;
    }""")
s = s.replace("""    reserve(512);
    return true;
}

void SdfBatch::shutdown() {""",
"""    if (!ensure_capacity(512)) {
        TB_LOG_ERROR("main", "sdf instance buffer allocation failed: {}",
                     SDL_GetError());
        shutdown();
        return false;
    }
    return true;
}

void SdfBatch::shutdown() {""")
s = s.replace("""    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    SDL_memcpy(map, strip, sizeof(strip));
    SDL_UnmapGPUTransferBuffer(device_, tb);""",
"""    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        shutdown();
        return false;
    }
    SDL_memcpy(map, strip, sizeof(strip));
    SDL_UnmapGPUTransferBuffer(device_, tb);""")
s = s.replace("""    void* map = SDL_MapGPUTransferBuffer(device_, upload_, true);
    SDL_memcpy(map, pending_.data(), pending_.size() * sizeof(SdfInstance));
    SDL_UnmapGPUTransferBuffer(device_, upload_);""",
"""    void* map = SDL_MapGPUTransferBuffer(device_, upload_, true);
    if (!map) {
        pending_.clear();
        return;
    }
    SDL_memcpy(map, pending_.data(), pending_.size() * sizeof(SdfInstance));
    SDL_UnmapGPUTransferBuffer(device_, upload_);""")
open(p, "w", encoding="utf-8").write(s)
print("batches ok")
