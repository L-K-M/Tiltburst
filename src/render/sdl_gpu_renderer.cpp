#include "render/sdl_gpu_renderer.h"

#include "core/log.h"
#include "core/time.h"
#include "platform/gpu_device.h"
#include "platform/paths.h"
#include "render/shader_load.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

// stb_image_write implementation lives here (single TU in tb_render).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace tb::render {

namespace {

// Palette bg0, neon palette (13-art-direction.md §2) — authored sRGB,
// written straight through the swapchain/UI variant.
constexpr SDL_FColor kClearColor{0x0D / 255.f, 0x02 / 255.f, 0x21 / 255.f, 1.0f};

#ifndef TB_SHADER_FALLBACK_DIR
#define TB_SHADER_FALLBACK_DIR ""
#endif

std::vector<std::filesystem::path> shader_search_dirs() {
    return {paths::base_dir() / "shaders", std::filesystem::path(TB_SHADER_FALLBACK_DIR)};
}

} // namespace

bool SdlGpuRenderer::init(const RendererConfig& cfg) {
    playfield_ = cfg.playfield_window;
    backglass_ = cfg.backglass_window;
    rotation_ = cfg.playfield_rotation;

    platform::SwapchainInit sc =
        platform::create_device_for_window(playfield_, cfg.debug_device, "auto");
    if (!sc.device) {
        return false;
    }
    device_ = sc.device;

    if (backglass_ != nullptr && !SDL_ClaimWindowForGPUDevice(device_, backglass_)) {
        TB_LOG_WARN("main", "backglass claim failed: {}", SDL_GetError());
        backglass_ = nullptr;
    }

    const std::filesystem::path shader_dir = find_shader_dir(shader_search_dirs());
    if (shader_dir.empty()) {
        TB_LOG_ERROR("main",
                     "no complete shader blob set found; see "
                     "/shaders/compiled and ADR-012");
        shutdown();
        return false;
    }

    if (!quads_.init(device_, sc.format, shader_dir)) {
        shutdown();
        return false;
    }
    return true;
}

void SdlGpuRenderer::destroy_device() {
    quads_.shutdown();
    if (offscreen_) {
        SDL_ReleaseGPUTexture(device_, offscreen_);
        offscreen_ = nullptr;
    }
    if (readback_) {
        SDL_ReleaseGPUTransferBuffer(device_, readback_);
        readback_ = nullptr;
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

void SdlGpuRenderer::render_playfield(const RenderFrame& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return;
    }

    SDL_GPUTexture* tex = nullptr;
    Uint32 w = 0;
    Uint32 h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, playfield_, &tex, &w, &h) || tex == nullptr) {
        // Minimized/occluded: skip the frame — every acquired command
        // buffer is submitted or cancelled exactly once (06 §4).
        SDL_CancelGPUCommandBuffer(cmd);
        ++stats_.playfield_skips;
        return;
    }

    const uint64_t frame_start = tb_now_ns();

    quads_.begin_frame(cmd, w, h, frame.snapshot ? double(frame.snapshot->tick) * 0.001 : 0.0);

    SDL_GPUColorTargetInfo target{};
    target.texture = tex;
    target.clear_color = kClearColor;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);

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
    SDL_SubmitGPUCommandBuffer(cmd);

    const float ms = float(tb_now_ns() - frame_start) / 1e6f;
    frame_ms_.push_back(ms);
    while (frame_ms_.size() > 240) {
        frame_ms_.pop_front();
    }
    ++stats_.frames;
}

bool SdlGpuRenderer::render_backglass(const BackglassFrame&) {
    if (backglass_ == nullptr) {
        return false;
    }
    // Backglass content arrives with the display system (M12); the
    // non-blocking ~30 Hz path is wired there.
    return false;
}

void SdlGpuRenderer::request_screenshot(const char* png_path) {
    // F12 capture path lands with debug draw at M3 (06 §15.1).
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

    SDL_GPUTextureCreateInfo tinfo{};
    tinfo.type = SDL_GPU_TEXTURETYPE_2D;
    tinfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tinfo.width = width;
    tinfo.height = height;
    tinfo.layer_count_or_depth = 1;
    tinfo.num_levels = 1;
    tinfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    offscreen_ = SDL_CreateGPUTexture(device_, &tinfo);
    if (!offscreen_) {
        TB_LOG_WARN("main", "offscreen target failed: {}", SDL_GetError());
        shutdown();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo rb{};
    rb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    rb.size = size_t(width) * height * 4;
    readback_ = SDL_CreateGPUTransferBuffer(device_, &rb);
    if (!readback_) {
        shutdown();
        return false;
    }

    const std::filesystem::path shader_dir = find_shader_dir(shader_search_dirs());
    if (shader_dir.empty() ||
        !quads_.init(device_, SDL_GPUTextureFormat(tinfo.format), shader_dir)) {
        shutdown();
        return false;
    }
    return true;
}

bool SdlGpuRenderer::render_offscreen_frame(const RenderFrame& frame) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        return false;
    }
    quads_.begin_frame(cmd,
                       offscreen_w_,
                       offscreen_h_,
                       frame.snapshot ? double(frame.snapshot->tick) * 0.001 : 0.0);

    SDL_GPUColorTargetInfo target{};
    target.texture = offscreen_;
    target.clear_color = kClearColor;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
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
    SDL_SubmitGPUCommandBuffer(cmd);

    ++stats_.frames;
    return true;
}

bool SdlGpuRenderer::write_png(const std::filesystem::path& out_png) {
    if (!device_ || !offscreen_ || !readback_) {
        return false;
    }
    SDL_WaitForGPUIdle(device_);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{offscreen_, 0, 0, 0, offscreen_w_, offscreen_h_, 1};
    SDL_GPUTextureTransferInfo dst{readback_, 0, offscreen_w_, offscreen_h_};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    void* map = SDL_MapGPUTransferBuffer(device_, readback_, false);
    const int ok = stbi_write_png(out_png.string().c_str(),
                                  int(offscreen_w_),
                                  int(offscreen_h_),
                                  4,
                                  map,
                                  int(offscreen_w_) * 4);
    SDL_UnmapGPUTransferBuffer(device_, readback_);

    if (!ok) {
        TB_LOG_WARN("main", "stbi_write_png failed for {}", out_png.string());
        return false;
    }
    TB_LOG_INFO("main", "wrote {}", out_png.string());
    return true;
}

} // namespace tb::render

namespace tb::render {

std::unique_ptr<IRenderer> make_sdl_gpu_renderer() {
    return std::make_unique<SdlGpuRenderer>();
}

} // namespace tb::render
