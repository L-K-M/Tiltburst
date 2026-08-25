#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// Shader loading with a hardcoded manifest (06-rendering.md §16.4): entry
// name, stage, formats — no runtime reflection.
namespace tb::render {

enum ShaderFormatBits : uint32_t {
    kShaderSpirv = 1 << 0,
    kShaderDxil = 1 << 1,
    kShaderMsl = 1 << 2,
};

enum class ShaderStage : uint8_t { Vertex, Fragment };

struct ShaderManifestEntry {
    const char* name; // stem, e.g. "sprite.vert"
    ShaderStage stage;
};

// Every shader named by the pipelines this build ships. Grows at M3+; the
// ManifestMatchesInstalledBlobs test asserts each entry has at least one
// compiled format installed next to the binary (or the committed blob).
inline constexpr ShaderManifestEntry kShaderManifest[] = {
    {"sprite.vert", ShaderStage::Vertex},
    {"sprite.frag", ShaderStage::Fragment},
};

constexpr const char* kBlobExtensions[3] = {".spv", ".dxil", ".msl"};

std::vector<uint8_t> load_shader_blob(const std::filesystem::path& path);

// First directory (in order) holding a readable blob for every manifest
// entry in at least one format; empty string when none qualifies.
std::filesystem::path find_shader_dir(const std::vector<std::filesystem::path>& search_dirs);

} // namespace tb::render
