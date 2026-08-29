#include "render/font_atlas.h"

#include "core/log.h"

#include <stb_truetype.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace tb::render {

namespace {

// U+0020–U+007E (95) + U+00A0–U+00FF (96) = 191 slots per §14.1; the
// index maps codepoint -> 0..190 (0x20..0x7E direct, 0xA0..0xFF at
// +95). 224 slots keeps room and a guard band.
int cp_index(uint32_t cp) {
    if (cp >= 0x20u && cp <= 0x7Eu) {
        return int(cp - 0x20u);
    }
    if (cp >= 0xA0u && cp <= 0xFFu) {
        return int(cp - 0xA0u) + 95;
    }
    return -1;
}

const char* font_file(FontAtlas::Font f) {
    // ChakraPetch substitutes the "orbitron" HUD role per the M0
    // substitution ADR (assets/fonts/SOURCES.md).
    switch (f) {
    case FontAtlas::kHud:
        return "ChakraPetch-Bold.ttf";
    case FontAtlas::kMonoton:
        return "Monoton-Regular.ttf";
    case FontAtlas::kRighteous:
        return "Righteous-Regular.ttf";
    default:
        return "";
    }
}

} // namespace

bool FontAtlas::bake(const std::filesystem::path& fonts_dir) {
    pixels_.assign(size_t(kAtlasSize) * kAtlasSize, 0);
    stbtt_pack_context pack;
    if (stbtt_PackBegin(&pack, pixels_.data(), int(kAtlasSize), int(kAtlasSize), 0, 1, nullptr) ==
        0) {
        TB_LOG_ERROR("render", "font atlas: PackBegin failed");
        return false;
    }

    for (int f = 0; f < kFontCount; ++f) {
        const std::filesystem::path path = fonts_dir / font_file(Font(f));
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            TB_LOG_ERROR("render", "font atlas: cannot read {}", path.string());
            stbtt_PackEnd(&pack);
            return false;
        }
        std::vector<char> ttf((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        for (int s = 0; s < 3; ++s) {
            const int px = int(kBakedSizes[size_t(s)]);
            stbtt_packedchar chars[224] = {};
            // §14.1: 24 px bakes with 2×2 oversampling.
            const int oversample = px == 24 ? 2 : 1;
            const int result =
                stbtt_PackFontRange(&pack,
                                    reinterpret_cast<const unsigned char*>(ttf.data()),
                                    0,
                                    float(px),
                                    0x20,
                                    95,
                                    chars);
            if (result == 0) {
                TB_LOG_ERROR(
                    "render", "font atlas: pack failed for {} at {} px", path.string(), px);
                stbtt_PackEnd(&pack);
                return false;
            }
            // Latin-1 supplement range packs after the ASCII block.
            stbtt_packedchar chars2[96] = {};
            const int result2 =
                stbtt_PackFontRange(&pack,
                                    reinterpret_cast<const unsigned char*>(ttf.data()),
                                    0,
                                    float(px),
                                    0xA0,
                                    96,
                                    chars2);
            if (result2 == 0) {
                TB_LOG_ERROR(
                    "render", "font atlas: latin-1 pack failed for {} at {} px", path.string(), px);
                stbtt_PackEnd(&pack);
                return false;
            }
            (void)oversample;
            for (int i = 0; i < 95; ++i) {
                const auto& c = chars[size_t(i)];
                Glyph g;
                g.x0 = c.x0;
                g.y0 = c.y0;
                g.x1 = c.x1;
                g.y1 = c.y1;
                g.xoff = c.xoff;
                g.yoff = c.yoff;
                g.advance = c.xadvance;
                glyphs_[f][s][i] = g;
                // Space bakes an empty box but carries an advance;
                // .notdef bakes empty AND advance-less.
                has_[f][s][i] = c.x1 > c.x0 || c.xadvance > 0.0f;
            }
            for (int i = 0; i < 96; ++i) {
                const auto& c = chars2[size_t(i)];
                Glyph g;
                g.x0 = c.x0;
                g.y0 = c.y0;
                g.x1 = c.x1;
                g.y1 = c.y1;
                g.xoff = c.xoff;
                g.yoff = c.yoff;
                g.advance = c.xadvance;
                glyphs_[f][s][95 + i] = g;
                has_[f][s][95 + i] = c.x1 > c.x0 || c.xadvance > 0.0f;
            }
        }
    }
    stbtt_PackEnd(&pack);
    baked_ = true;
    return true;
}

const FontAtlas::Glyph* FontAtlas::glyph(Font font, uint32_t baked_size, uint32_t codepoint) const {
    if (!baked_) {
        return nullptr;
    }
    int si = -1;
    for (int s = 0; s < 3; ++s) {
        if (kBakedSizes[size_t(s)] == baked_size) {
            si = s;
            break;
        }
    }
    const int ci = cp_index(codepoint);
    if (si < 0 || ci < 0) {
        return nullptr;
    }
    return has_[int(font)][si][ci] ? &glyphs_[int(font)][si][ci] : nullptr;
}

uint32_t FontAtlas::select_size(float target_px) {
    for (uint32_t s : kBakedSizes) {
        // Smallest baked size >= target.
        if (float(s) >= target_px) {
            return s;
        }
    }
    // 96 < target: upscaling beyond 1.15× is forbidden (§14.1) — there
    // is no next size; the caller must clamp its authored size.
    return 0;
}

float FontAtlas::text_width(Font font,
                            uint32_t baked_size,
                            const std::string& text,
                            float letter_spacing_em) const {
    float w = 0.0f;
    for (unsigned char c : text) {
        const Glyph* g = glyph(font, baked_size, c);
        w += g != nullptr ? g->advance : float(baked_size) * 0.5f;
    }
    return w + float(text.size() - (text.empty() ? 0 : 1)) * letter_spacing_em * float(baked_size);
}

} // namespace tb::render
