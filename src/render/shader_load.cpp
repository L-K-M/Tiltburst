#include "render/shader_load.h"

#include <cstdio>

namespace tb::render {

std::vector<uint8_t> load_shader_blob(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? size_t(size) : 0);
    const size_t read = size > 0 ? std::fread(bytes.data(), 1, size_t(size), f) : 0;
    std::fclose(f);
    bytes.resize(read);
    return bytes;
}

namespace {

bool entry_has_blob(const std::filesystem::path& dir, const char* name) {
    for (const char* ext : kBlobExtensions) {
        std::error_code ec;
        if (std::filesystem::exists(dir / (std::string(name) + ext), ec)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::filesystem::path find_shader_dir(const std::vector<std::filesystem::path>& search_dirs) {
    for (const auto& dir : search_dirs) {
        bool complete = true;
        for (const ShaderManifestEntry& e : kShaderManifest) {
            if (!entry_has_blob(dir, e.name)) {
                complete = false;
                break;
            }
        }
        if (complete) {
            return dir;
        }
    }
    return {};
}

} // namespace tb::render
