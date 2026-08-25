#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

// Repo-relative test data resolution (16-testing-ci.md §2, normative).
// Every repo-relative path in every test goes through this helper; the
// process working directory must never be consulted.
namespace tb::test {

inline std::filesystem::path data_path(const std::filesystem::path& rel) {
    const char* root = std::getenv("TB_SOURCE_DIR");
    std::filesystem::path base =
        root ? std::filesystem::path(root) : std::filesystem::path(TB_SOURCE_DIR);
    return base / rel;
}

} // namespace tb::test
