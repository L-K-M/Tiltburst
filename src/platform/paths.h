#pragma once

#include <filesystem>
#include <string>

// Filesystem locations (05-engine-core.md §1 steps 4–5).
namespace tb::paths {

// Initializes the per-user pref directory (SDL_GetPrefPath). Returns false
// when SDL could not provide one (hard failure per §1 step 4).
bool init_pref();

const std::filesystem::path& pref();
std::filesystem::path logs_dir();

// Directory containing the executable; shader blobs install next to it
// (<base>/shaders, 06-rendering.md §16.4).
std::filesystem::path base_dir();

} // namespace tb::paths
