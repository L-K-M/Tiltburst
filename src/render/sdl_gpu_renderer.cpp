#include "render/sdl_gpu_renderer.h"

#include "core/log.h"
#include "core/time.h"
#include "platform/gpu_device.h"
#include "platform/paths.h"
#include "render/shader_load.h"
#include "render/view.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

// stb_image_write implementation lives here (single TU in tb_render).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace tb::render {

namespace {

// Palette bg0 (13-art-direction.md §2) — authored sRGB for the UI variant.
constexpr SDL_FColor kClearColor{0x0D / 255.f, 0x02 / 255.f, 0x21 / 255.f, 1.0f};
constexpr float kOverlayColor[4] = {0.0f, 0xE5 / 255.f, 1.0f, 1.0f};

#ifndef TB_SHADER_FALLBACK_DIR
#define TB_SHADER_FALLBACK_DIR ""
#endif

std::vector<std::filesystem::path> shader_search_dirs() {
    return {paths::base_dir() / "shaders", std::filesystem::path(TB_SHADER_FALLBACK_DIR)};
}

} // namespace

// ---------------------------------------------------------------------------
// F2 debug instances (06 §16.1): colliders + ball bodies, table space.
// ---------------------------------------------------------------------------

void SdlGpuRenderer::collider_instances(const sim::Collider& c,
                                        float ppm,
                                        std::vector<SdfInstance>& out) {
    const float half_w = 1.5f / ppm * 0.5f; // 1.5 px stroke in meters

    auto seg_color = [&](float* rgb) {
        rgb[0] = 0.0f; // #00FF66
        rgb[1] = 1.0f;
        rgb[2] = 0.4f;
    };
    auto arc_color = [&](float* rgb) {
        rgb[0] = 0.0f; // #00CCFF
        rgb[1] = 0.8f;
        rgb[2] = 1.0f;
    };

    SdfInstance i{};
    i.glow_radius = 0.0f;

    if (c.kind == sim::Collider::Kind::Segment) {
        const sim::Vec2 d = c.b - c.a;
        const float len = length(d);
        i.cx = (c.a.x + c.b.x) * 0.5f;
        i.cy = (c.a.y + c.b.y) * 0.5f;
        i.rot = std::atan2(d.y, d.x);
        i.kind = kSdfCapsule;
        i.p0 = -len * 0.5f;
        i.p1 = 0.0f;
        i.p2 = len * 0.5f;
        i.p3 = 0.0f;
        i.p4 = half_w;
        i.hx = len * 0.5f + half_w + 0.001f;
        i.hy = half_w + 0.001f;
        seg_color(i.stroke);
        i.stroke[3] = half_w * 2.0f;
        out.push_back(i);
        return;
    }

    if (c.kind == sim::Collider::Kind::Point) {
        i.cx = c.a.x;
        i.cy = c.a.y;
        i.kind = kSdfCircle;
        i.p0 = c.radius + half_w;
        i.hx = i.hy = i.p0 + 0.002f;
        arc_color(i.stroke);
        i.stroke[3] = half_w * 2.0f;
        out.push_back(i);
        return;
    }

    // Arc: a0/a1 ride p0/p1; radius/thickness p2/p3 (shader §8 mapping).
    i.cx = c.a.x;
    i.cy = c.a.y;
    i.kind = kSdfArc;
    i.p0 = c.a0;
    i.p1 = c.a1;
    i.p2 = c.radius;
    i.p3 = half_w;
    i.hx = i.hy = c.radius + half_w + 0.002f;
    arc_color(i.stroke);
    i.stroke[3] = half_w * 2.0f;
    out.push_back(i);
}

void SdlGpuRenderer::ball_instances(const SimSnapshot& snap, std::vector<SdfInstance>& out) {
    for (uint32_t i = 0; i < snap.ball_count; ++i) {
        const auto& b = snap.balls[i];
        SdfInstance ball{};
        ball.cx = b.x;
        ball.cy = b.y;
        ball.kind = kSdfBall;
        ball.p0 = 0.0135f;
        ball.hx = ball.hy = 0.0135f + 0.002f;
        // Fake-chrome placeholder gradient (M13 refines per §11).
        for (int k = 0; k < 3; ++k) {
            ball.fill0[k] = 0.75f;
            ball.fill1[k] = 0.25f;
        }
        ball.fill0[3] = ball.fill1[3] = 1.0f;
        ball.grad[1] = -1.0f;
        ball.grad[2] = 0.027f;
        ball.grad[3] = 1.0f;
        out.push_back(ball);
    }
}

// ---------------------------------------------------------------------------

SdlGpuRenderer::~SdlGpuRenderer() {
    shutdown();
}

bool SdlGpuRenderer::init(const RendererConfig& cfg) {
    playfield_ = cfg.playfield_window;
    backglass_ = cfg.backglass_window;
    rotation_ = cfg.playfield_rotation;
    bloom_strength_ = cfg.bloom_strength;
    crt_ = cfg.crt;

    const platform::SwapchainInit sc =
        platform::create_device_for_window(playfield_, cfg.debug_device, "auto");
    if (!sc.device) {
        return false;
    }
    device_ = sc.device;

    if (backglass_ != nullptr && !SDL_ClaimWindowForGPUDevice(device_, backglass_)) {
        TB_LOG_WARN("main", "backglass claim failed: {}", SDL_GetError());
        backglass_ = nullptr;
    }

    const std::filesystem::path dir = find_shader_dir(shader_search_dirs());
    if (dir.empty()) {
        TB_LOG_ERROR("main",
                     "no complete shader blob set found; see "
                     "/shaders/compiled and ADR-012");
        shutdown();
        return false;
    }

    if (!quads_.init(device_, sc.format, dir) || !sdf_.init(device_, sc.format, dir) ||
        !present_.init(device_, sc.format, dir)) {
        shutdown();
        return false;
    }
    // §12: the bloom chain is best-effort — a missing blob degrades
    // to Quality::Off (the composite's bloom term is multiplied by 0
    // via a null bind, so the plain path renders).
    if (!bloom_.init(device_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dir)) {
        TB_LOG_WARN("render", "bloom chain unavailable; Quality::Off fallback");
        bloom_.set_quality(BloomChain::Quality::Off);
    }
    scene_w_ = 0; // recreate on first frame via ensure_scene
    scene_h_ = 0;
    return true;
}

void SdlGpuRenderer::destroy_device() {
    quads_.shutdown();
    sdf_.shutdown();
    present_.shutdown();
    bloom_.shutdown();
    if (scene_) {
        SDL_ReleaseGPUTexture(device_, scene_);
        scene_ = nullptr;
    }
    if (capture_) {
        SDL_ReleaseGPUTexture(device_, capture_);
        capture_ = nullptr;
    }
    if (readback_) {
        SDL_ReleaseGPUTransferBuffer(device_, readback_);
        readback_ = nullptr;
    }
    if (offscreen_) {
        SDL_ReleaseGPUTexture(device_, offscreen_);
        offscreen_ = nullptr;
    }
    if (smoke_readback_) {
        SDL_ReleaseGPUTransferBuffer(device_, smoke_readback_);
        smoke_readback_ = nullptr;
    }
    if (device_) {
        SDL_WaitForGPUIdle(device_);
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    playfield_ = nullptr;
    backglass_ = nullptr;
}

void SdlGpuRenderer::shutdown() {
    destroy_device();
}

bool SdlGpuRenderer::ensure_scene(SDL_GPUTextureFormat) {
    if (scene_ && view_.scene_w == scene_w_ && view_.scene_h == scene_h_) {
        return true;
    }
    if (scene_) {
        SDL_ReleaseGPUTexture(device_, scene_);
        scene_ = nullptr;
    }
    scene_w_ = view_.scene_w;
    scene_h_ = view_.scene_h;

    // §12: bloom level targets follow the scene size.
    if (bloom_.quality() != BloomChain::Quality::Off) {
        bloom_.ensure_targets(scene_w_, scene_h_);
    }

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = std::max(1u, scene_w_);
    ti.height = std::max(1u, scene_h_);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    // RGBA16F per §7; consumed by the bloom chain at M13.
    ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    scene_ = SDL_CreateGPUTexture(device_, &ti);
    return scene_ != nullptr;
}

void SdlGpuRenderer::draw_scene(SDL_GPUCommandBuffer* cmd,
                                const RenderFrame& frame,
                                double time_s) {
    float table_clip[16];
    table_to_clip(float(scene_w_), float(scene_h_), table_clip);

    quads_.begin_frame(cmd, scene_w_, scene_h_, time_s);
    sdf_.begin_frame(cmd, table_clip, scene_w_, scene_h_, time_s);

    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = scene_;
    tgt.clear_color = kClearColor;
    tgt.load_op = SDL_GPU_LOADOP_CLEAR;
    tgt.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);

    debug_instances_.clear();

    // M13a art: below-ball instances first (layers z < 100).
    if (frame.art != nullptr && frame.art->below_count > 0) {
        const SdfInstance* below = static_cast<const SdfInstance*>(frame.art->below);
        for (uint32_t i = 0; i < frame.art->below_count; ++i) {
            sdf_.push(below[i]);
        }
    }

    if (frame.show_colliders && frame.debug_colliders != nullptr) {
        for (uint32_t i = 0; i < frame.debug_collider_count; ++i) {
            collider_instances(frame.debug_colliders[i], view_.ppm, debug_instances_);
        }
    }
    if (frame.snapshot != nullptr) {
        ball_instances(*frame.snapshot, debug_instances_);
    }

    // Above-ball art (layers z >= 100: ramps, wireforms) draws after
    // the ball so the chrome ball reads through.
    if (frame.art != nullptr && frame.art->above_count > 0) {
        const SdfInstance* above = static_cast<const SdfInstance*>(frame.art->above);
        for (uint32_t i = 0; i < frame.art->above_count; ++i) {
            sdf_.push(above[i]);
        }
    }

    sdf_.upload_and_draw(pass);

    if (frame.quad_count > 0) {
        for (uint32_t i = 0; i < frame.quad_count; ++i) {
            quads_.push(frame.quads[i].cx,
                        frame.quads[i].cy,
                        frame.quads[i].hx,
                        frame.quads[i].hy,
                        frame.quads[i].r,
                        frame.quads[i].g,
                        frame.quads[i].b,
                        frame.quads[i].a);
        }
        quads_.upload_and_draw(pass);
    }

    SDL_EndGPURenderPass(pass);
}

void SdlGpuRenderer::render_playfield(const RenderFrame& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return;
    }

    SDL_GPUTexture* tex = nullptr;
    Uint32 w = 0;
    Uint32 h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, playfield_, &tex, &w, &h) || tex == nullptr) {
        // Minimized/occluded: skip — every acquired buffer is submitted or
        // cancelled exactly once (06 §4).
        SDL_CancelGPUCommandBuffer(cmd);
        ++stats_.playfield_skips;
        return;
    }

    const uint64_t frame_start = tb_now_ns();

    view_ = compute_view(uint32_t(int(rotation_)), w, h, kReferenceTableW, kReferenceTableH);
    if (!ensure_scene(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    sim_time_s_ = frame.snapshot ? double(frame.snapshot->tick) * 0.001 : sim_time_s_;
    draw_scene(cmd, frame, sim_time_s_);

    // §12: the bloom chain (11 passes at <= 1/2 res), then the §12.5
    // composite with the combined bloom0.
    if (bloom_.quality() != BloomChain::Quality::Off) {
        bloom_.record(cmd, scene_);
    }
    present_.build_corners(view_, w, h);
    present_.add_pass(cmd,
                      scene_,
                      bloom_.bloom0(),
                      tex,
                      bloom_.quality() == BloomChain::Quality::Off ? 0.0f : bloom_strength_,
                      crt_,
                      scene_w_,
                      scene_h_);

    SDL_SubmitGPUCommandBuffer(cmd);

    const float ms = float(tb_now_ns() - frame_start) / 1e6f;
    frame_ms_.push_back(ms);
    while (frame_ms_.size() > 240) {
        frame_ms_.pop_front();
    }
    stats_.sdf_instances = uint32_t(debug_instances_.size());
    ++stats_.frames;

    if (!pending_screenshot_.empty()) {
        capture_to_png(pending_screenshot_);
        pending_screenshot_.clear();
    }
}

bool SdlGpuRenderer::render_backglass(const BackglassFrame& frame) {
    if (backglass_ == nullptr) {
        return false;
    }
    // sim_time_s_ is assigned only from snapshot->tick (unsigned) * ms,
    // so it can never be negative; a clamp here guarded nothing (and
    // std::max(NaN, 0.0) returns NaN anyway — it was never a NaN guard).
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        ++stats_.backglass_skips;
        return false;
    }
    SDL_GPUTexture* tex = nullptr;
    Uint32 w = 0;
    Uint32 h = 0;
    // 07 §7: NON-BLOCKING acquire — no texture means skip the frame
    // (and cancel the buffer exactly once); only the playfield may
    // wait.
    if (!SDL_AcquireGPUSwapchainTexture(cmd, backglass_, &tex, &w, &h) || tex == nullptr) {
        SDL_CancelGPUCommandBuffer(cmd);
        ++stats_.backglass_skips;
        return false;
    }
    // The canvas is a fixed 640x512 design surface; letterbox into the
    // swapchain by scaling the pixel-space ortho to the actual extent.
    SDL_GPUColorTargetInfo target{};
    target.texture = tex;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (pass == nullptr) {
        SDL_CancelGPUCommandBuffer(cmd);
        ++stats_.backglass_skips;
        return false;
    }

    quads_.begin_frame(cmd, w, h, sim_time_s_);
    quads_.reserve(frame.quad_count);
    const float scale = std::min(float(w) / float(BackglassFrame::kCanvasW),
                                 float(h) / float(BackglassFrame::kCanvasH));
    const float ox = (float(w) - scale * float(BackglassFrame::kCanvasW)) * 0.5f;
    const float oy = (float(h) - scale * float(BackglassFrame::kCanvasH)) * 0.5f;
    for (uint32_t i = 0; i < frame.quad_count; ++i) {
        const QuadInstance& q = frame.quads[i];
        quads_.push(
            ox + q.cx * scale, oy + q.cy * scale, q.hx * scale, q.hy * scale, q.r, q.g, q.b, q.a);
    }
    quads_.upload_and_draw(pass);
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

void SdlGpuRenderer::request_screenshot(const char* png_path) {
    pending_screenshot_ = png_path != nullptr ? png_path : "";
}

std::string SdlGpuRenderer::backend_name() const {
    return device_ ? SDL_GetGPUDeviceDriver(device_) : "none";
}

bool SdlGpuRenderer::init_offscreen(uint32_t width, uint32_t height, bool debug_mode) {
    const Uint32 formats = platform::available_shader_formats();
    if (formats == 0) {
        TB_LOG_WARN("main", "no shader blobs; cannot create GPU device");
        return false;
    }
    device_ = SDL_CreateGPUDevice(formats, debug_mode, nullptr);
    if (!device_) {
        TB_LOG_WARN("main", "no usable GPU backend: {}", SDL_GetError());
        return false;
    }
    SDL_SetGPUAllowedFramesInFlight(device_, 1);

    offscreen_w_ = width;
    offscreen_h_ = height;

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = width;
    ti.height = height;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    offscreen_ = SDL_CreateGPUTexture(device_, &ti);
    if (!offscreen_) {
        TB_LOG_WARN("main", "offscreen target failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo rb{};
    rb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    rb.size = size_t(width) * height * 4;
    smoke_readback_ = SDL_CreateGPUTransferBuffer(device_, &rb);
    if (!smoke_readback_) {
        shutdown();
        return false;
    }

    const std::filesystem::path dir = find_shader_dir(shader_search_dirs());
    if (dir.empty() || !quads_.init(device_, ti.format, dir) ||
        !sdf_.init(device_, ti.format, dir)) {
        shutdown();
        return false;
    }

    view_.rotation = 0;
    view_.scene_w = width;
    view_.scene_h = height;
    return true;
}

bool SdlGpuRenderer::render_offscreen_frame(const RenderFrame& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return false;
    }
    draw_scene(cmd, frame, frame.snapshot ? double(frame.snapshot->tick) * 0.001 : 0.0);
    ++stats_.frames;
    return true;
}

bool SdlGpuRenderer::write_png(const std::filesystem::path& out_png) {
    if (!device_ || !offscreen_ || !smoke_readback_) {
        return false;
    }
    SDL_WaitForGPUIdle(device_);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{offscreen_, 0, 0, 0, 0, offscreen_w_, offscreen_h_, 1};
    SDL_GPUTextureTransferInfo dst{smoke_readback_, 0, offscreen_w_, offscreen_h_};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    void* map = SDL_MapGPUTransferBuffer(device_, smoke_readback_, false);
    if (!map) {
        return false;
    }
    const int ok = stbi_write_png(out_png.string().c_str(),
                                  int(offscreen_w_),
                                  int(offscreen_h_),
                                  4,
                                  map,
                                  int(offscreen_w_) * 4);
    SDL_UnmapGPUTransferBuffer(device_, smoke_readback_);

    if (!ok) {
        TB_LOG_WARN("main", "stbi_write_png failed for {}", out_png.string());
        return false;
    }
    TB_LOG_INFO("main", "wrote {}", out_png.string());
    return true;
}

bool SdlGpuRenderer::capture_to_png(const std::filesystem::path& png) {
    // F12 (§15.1): re-render one cached frame into an RGBA8 target sized to
    // the scene and download it — never copies the swapchain.
    if (scene_w_ == 0 || scene_h_ == 0 || !device_) {
        return false;
    }
    if (capture_w_ != scene_w_ || capture_h_ != scene_h_ || !capture_ || !readback_) {
        if (capture_) {
            SDL_ReleaseGPUTexture(device_, capture_);
            capture_ = nullptr;
        }
        if (readback_) {
            SDL_ReleaseGPUTransferBuffer(device_, readback_);
            readback_ = nullptr;
        }
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = scene_w_;
        ti.height = scene_h_;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        capture_ = SDL_CreateGPUTexture(device_, &ti);

        SDL_GPUTransferBufferCreateInfo rb{};
        rb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        rb.size = size_t(scene_w_) * scene_h_ * 4;
        readback_ = SDL_CreateGPUTransferBuffer(device_, &rb);

        capture_w_ = scene_w_;
        capture_h_ = scene_h_;
    }
    if (!capture_ || !readback_) {
        return false;
    }

    SDL_WaitForGPUIdle(device_);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return false;
    }

    draw_scene(cmd, last_, last_.snapshot ? double(last_.snapshot->tick) * 0.001 : 0.0);
    // §12.5 goldens: CRT always off in tb_screenshot (13 §10);
    // capture uses bloom strength 0 (§15.2: goldens must not depend
    // on quality settings) — but the chain itself may run; the
    // composite samples with strength 0.
    present_.add_pass(cmd, scene_, bloom_.bloom0(), capture_, 0.0f, false, scene_w_, scene_h_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{capture_, 0, 0, 0, 0, capture_w_, capture_h_, 1};
    SDL_GPUTextureTransferInfo dst{readback_, 0, capture_w_, capture_h_};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    void* map = SDL_MapGPUTransferBuffer(device_, readback_, false);
    if (!map) {
        return false;
    }
    const int ok = stbi_write_png(
        png.string().c_str(), int(capture_w_), int(capture_h_), 4, map, int(capture_w_) * 4);
    SDL_UnmapGPUTransferBuffer(device_, readback_);
    TB_LOG_INFO("main", "screenshot {}", ok != 0 ? png.string() : std::string("failed"));
    return ok != 0;
}

} // namespace tb::render

namespace tb::render {

std::unique_ptr<IRenderer> make_sdl_gpu_renderer() {
    return std::make_unique<SdlGpuRenderer>();
}

} // namespace tb::render
