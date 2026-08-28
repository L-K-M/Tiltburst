#include "platform/window.h"

namespace tb::platform {

WindowPtr create_window(const std::string& title, int width, int height) {
    SDL_Window* w = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_RESIZABLE);
    return WindowPtr(w);
}

WindowPtr create_fullscreen_window(const std::string& title,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t display) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(
        p, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(
        p, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, double(width));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, double(height));
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* w = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    if (w != nullptr) {
        // NULL = borderless desktop fullscreen (never an exclusive mode).
        SDL_SetWindowFullscreenMode(w, nullptr);
    }
    return WindowPtr(w);
}

} // namespace tb::platform
