#include "platform/gpu_device.h"

#include "core/log.h"
#include "platform/paths.h"

#include <algorithm>
#include <array>
#include <filesystem>

namespace tb::platform {

namespace {

struct BlobFormat {
    const char* extension;
    Uint32 flag;
};

constexpr std::array<BlobFormat, 3> kBlobFormats{{
    {".spv", SDL_GPU_SHADERFORMAT_SPIRV},
    {".dxil", SDL_GPU_SHADERFORMAT_DXIL},
    {".msl", SDL_GPU_SHADERFORMAT_MSL},
}};

} // namespace

Uint32 available_shader_formats() {
    const std::filesystem::path dir = paths::base_dir() / "shaders";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return 0;
    }

    Uint32 mask = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        for (const BlobFormat& f : kBlobFormats) {
            if (name.size() > 4 && name.compare(name.size() - 4, 4, f.extension) == 0) {
                mask |= f.flag;
            }
        }
    }
    return mask;
}

SwapchainInit create_device_for_window(SDL_Window* window,
                                       bool debug_mode,
                                       const std::string& present_preference) {
    SwapchainInit out;

    const Uint32 formats = available_shader_formats();
    if (formats == 0) {
        TB_LOG_ERROR("main",
                     "no shader blobs found under {}shaders; cannot "
                     "create a GPU device",
                     paths::base_dir().string());
        return out;
    }

    SDL_GPUDevice* dev = SDL_CreateGPUDevice(formats, /*debug_mode=*/debug_mode, /*name=*/nullptr);
    if (!dev) {
        TB_LOG_ERROR("main", "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return out;
    }

    // Frames-in-flight is 1 device-wide (canon §5.4; R2 latency cap).
    SDL_SetGPUAllowedFramesInFlight(dev, 1);

    if (!SDL_ClaimWindowForGPUDevice(dev, window)) {
        TB_LOG_ERROR("main", "SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        SDL_DestroyGPUDevice(dev);
        return out;
    }

    bool mailbox = false;
    if (present_preference != "vsync") {
        mailbox = SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX);
    }
    const SDL_GPUPresentMode mode =
        mailbox ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    if (!SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
        TB_LOG_WARN("main", "SDL_SetGPUSwapchainParameters failed: {}", SDL_GetError());
    }
    TB_LOG_INFO("main", "playfield swapchain: present_mode={}", mailbox ? "MAILBOX" : "VSYNC");

    out.device = dev;
    out.format = SDL_GetGPUSwapchainTextureFormat(dev, window);
    return out;
}

} // namespace tb::platform
