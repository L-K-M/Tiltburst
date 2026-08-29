#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

namespace tb::platform {

struct WindowDeleter {
    void operator()(SDL_Window* w) const {
        if (w != nullptr) {
            SDL_DestroyWindow(w);
        }
    }
};

using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;

// Windowed dev mode: a resizable portrait window (default 540x1080,
// 04-milestones.md M1). Borderless fullscreen arrives with the display
// system at M12.
WindowPtr create_window(const std::string& title, int width, int height);
// 07 §7: borderless desktop fullscreen centered on `display`
// (SDL_DisplayID). Never an exclusive mode (§7 binding note).
WindowPtr create_fullscreen_window(const std::string& title,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t display);

} // namespace tb::platform
