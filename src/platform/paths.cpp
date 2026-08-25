#include "platform/paths.h"

#include <SDL3/SDL_filesystem.h>

#include <cstdlib>

namespace tb::paths {

namespace {
std::filesystem::path g_pref;
std::filesystem::path g_base;
} // namespace

bool init_pref() {
    char* p = SDL_GetPrefPath("tiltburst", "tiltburst");
    if (!p) {
        return false;
    }
    g_pref = p;
    SDL_free(p);

    std::error_code ec;
    std::filesystem::create_directories(g_pref / "logs", ec);
    return !ec;
}

const std::filesystem::path& pref() {
    return g_pref;
}

std::filesystem::path logs_dir() {
    return g_pref / "logs";
}

std::filesystem::path base_dir() {
    if (g_base.empty()) {
        if (const char* p = SDL_GetBasePath()) {
            g_base = p;
        }
    }
    return g_base.empty() ? std::filesystem::current_path() : g_base;
}

} // namespace tb::paths
