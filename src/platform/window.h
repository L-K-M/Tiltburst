#pragma once

#include <SDL3/SDL.h>

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

} // namespace tb::platform
