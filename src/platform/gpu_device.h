#pragma once

#include <SDL3/SDL_gpu.h>

#include <memory>
#include <string>

// GPU device bootstrap (05-engine-core.md §1 step 9, 06-rendering.md §3).
namespace tb::platform {

// Runtime scan of <base>/shaders for the blob formats actually present,
// intersected with the three SDL knows (06 §16.4). Returns an
// SDL_GPUShaderFormat mask; 0 means no usable backend.
Uint32 available_shader_formats();

struct GpuContextDeleter {
    void operator()(SDL_GPUDevice* d) const {
        if (d != nullptr) {
            SDL_DestroyGPUDevice(d);
        }
    }
};

using GpuDevicePtr = std::unique_ptr<SDL_GPUDevice, GpuContextDeleter>;

struct SwapchainInit {
    SDL_GPUDevice* device = nullptr;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID; // post-claim
};

// Creates the device from the on-disk format mask, claims the window, and
// applies the canon swapchain parameters: SDR composition,
// MAILBOX-else-VSYNC per `present_preference` ("auto"|"mailbox"|"vsync"),
// frames-in-flight 1 device-wide. The chosen present mode is returned and
// logged by the caller.
SwapchainInit create_device_for_window(SDL_Window* window,
                                       bool debug_mode,
                                       const std::string& present_preference);

} // namespace tb::platform
