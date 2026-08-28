#include "platform/display_detect.h"

#include <SDL3/SDL.h>

namespace tb::platform {

// 07 §2: enumerate in SDL_GetDisplays order. DesktopDisplayMode —
// NEVER Current (a transient mode set by another app must not skew
// detection).
bool enumerate_displays(std::vector<DisplayInfo>& out) {
    out.clear();
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    if (ids == nullptr || n <= 0) {
        SDL_free(ids);
        return false;
    }
    for (int i = 0; i < n; ++i) {
        DisplayInfo d;
        d.index = int(out.size()); // position in SDL order
        d.sdl_id = uint32_t(ids[i]);
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i]);
        if (m != nullptr) {
            d.w = m->w;
            d.h = m->h;
            d.refresh_hz = m->refresh_rate > 0.0f ? m->refresh_rate : 0.0f;
        }
        const char* name = SDL_GetDisplayName(ids[i]);
        d.name = name != nullptr ? name : "";
        out.push_back(std::move(d)); // kept even without a mode: indices
                                     // never shift (0x0 scores last)
    }
    SDL_free(ids);
    return !out.empty();
}

} // namespace tb::platform
