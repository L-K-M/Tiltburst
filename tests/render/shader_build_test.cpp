#include "render/shader_load.h"
#include "support/data_path.h"

#include <gtest/gtest.h>

#ifndef TB_SHADER_BUILD_DIR
#define TB_SHADER_BUILD_DIR ""
#endif

// ShaderBuild.ManifestMatchesInstalledBlobs: every shader named in the C++
// manifest has at least one compiled format installed next to the binary,
// or an ADR-012 committed blob (04-milestones.md M1).
TEST(unit_shader_build, manifest_matches_installed_blobs) {
    std::vector<std::filesystem::path> dirs;
    if (TB_SHADER_BUILD_DIR[0] != '\0') {
        dirs.emplace_back(TB_SHADER_BUILD_DIR);
    }
    dirs.push_back(tb::test::data_path(std::filesystem::path("shaders") / "compiled"));

    const std::filesystem::path dir = tb::render::find_shader_dir(dirs);
    ASSERT_FALSE(dir.empty()) << "no directory holds a blob for every manifest entry";

    for (const tb::render::ShaderManifestEntry& entry : tb::render::kShaderManifest) {
        bool any_format = false;
        for (const char* ext : tb::render::kBlobExtensions) {
            const std::filesystem::path blob = dir / (std::string(entry.name) + ext);
            if (std::filesystem::exists(blob)) {
                EXPECT_GT(std::filesystem::file_size(blob), 0u) << blob.string();
                any_format = true;
            }
        }
        EXPECT_TRUE(any_format) << entry.name << " has no compiled format";
    }
}
