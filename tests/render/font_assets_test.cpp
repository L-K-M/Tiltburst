#include <gtest/gtest.h>

#include <picosha2.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "support/data_path.h"

#include <stb_truetype.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// 13-art-direction.md §5.1: role → vendored file. The orbitron role is
// filled by the documented substitution (ADR-015); logical names unchanged.
struct FontSpec {
    const char* file;
    bool full_ascii; // U+0020–U+007E required (Monoton is uppercase-only)
};

constexpr std::array<FontSpec, 3> kFonts{{
    {"ChakraPetch-Bold.ttf", true},
    {"Monoton-Regular.ttf", false},
    {"Righteous-Regular.ttf", true},
}};

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string sha256_hex(const std::vector<unsigned char>& bytes) {
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    return picosha2::bytes_to_hex_string(digest.begin(), digest.end());
}

// SHA256SUMS lines are `sha256sum -c` format: "<64 hex>  <filename>".
struct SumsLine {
    std::string hash;
    std::string file;
};

std::vector<SumsLine> parse_sha256sums(const std::string& text) {
    std::vector<SumsLine> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Tolerate a CRLF checkout: strip any trailing carriage return.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.size() < 66 || line[64] != ' ') {
            continue;
        }
        out.push_back({line.substr(0, 64), line.substr(66)});
    }
    return out;
}

} // namespace

TEST(FontAssets, VendoredFontsPresentAndParse) {
    // Integrity pin: every committed .ttf matches its recorded SHA-256.
    const std::string sums_text = read_file(tb::test::data_path("assets/fonts/SHA256SUMS"));
    const std::vector<SumsLine> sums = parse_sha256sums(sums_text);
    ASSERT_EQ(sums.size(), kFonts.size());

    for (const FontSpec& spec : kFonts) {
        const std::filesystem::path path =
            tb::test::data_path(std::string("assets/fonts/") + spec.file);

        SCOPED_TRACE(spec.file);
        ASSERT_TRUE(std::filesystem::exists(path));

        const std::vector<unsigned char> bytes = read_bytes(path);
        ASSERT_FALSE(bytes.empty());
        EXPECT_EQ(sha256_hex(bytes).size(), 64u);

        const auto recorded = std::find_if(
            sums.begin(), sums.end(), [&](const SumsLine& s) { return s.file == spec.file; });
        ASSERT_NE(recorded, sums.end());
        EXPECT_EQ(sha256_hex(bytes), recorded->hash);

        // Parse with stb_truetype and require basic metrics/coverage.
        stbtt_fontinfo info{};
        ASSERT_NE(stbtt_InitFont(&info, bytes.data(), 0), 0);

        int ascent = 0;
        int descent = 0;
        int line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        EXPECT_GT(ascent, 0);

        for (int c = 'A'; c <= 'Z'; ++c) {
            EXPECT_NE(stbtt_FindGlyphIndex(&info, c), 0) << "codepoint " << c;
        }
        for (int c = '0'; c <= '9'; ++c) {
            EXPECT_NE(stbtt_FindGlyphIndex(&info, c), 0) << "codepoint " << c;
        }
        if (spec.full_ascii) {
            for (int c = 0x20; c <= 0x7E; ++c) {
                EXPECT_NE(stbtt_FindGlyphIndex(&info, c), 0) << "codepoint " << c;
            }
        }

        // OFL 1.1 requires the license to travel with the binary. The
        // license file is named per family (no style suffix).
        const std::string ofl_name = spec.file;
        const size_t dash = ofl_name.find('-');
        const std::string ofl_path = "assets/fonts/" + ofl_name.substr(0, dash) + "-OFL.txt";
        const std::string ofl_text = read_file(tb::test::data_path(ofl_path));
        EXPECT_FALSE(ofl_text.empty());
    }
}
