#include "platform/window.h"

namespace tb::platform {

WindowPtr create_window(const std::string& title, int width, int height) {
    SDL_Window* w = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_RESIZABLE);
    return WindowPtr(w);
}

} // namespace tb::platform
