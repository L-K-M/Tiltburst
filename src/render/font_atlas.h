#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tb::render {

// The single 2048×2048 R8 font atlas (06-rendering.md §14.1): the three
// vendored OFL faces at pixel sizes 24/48/96, codepoints U+0020–U+007E
// and U+00A0–U+00FF; 24 px bakes with 2×2 oversampling. Baked once at
// startup; a pack failure is fatal (it fits with ≥ 15% slack — failure
// means a corrupt font).
class FontAtlas {
public:
    enum Font : uint8_t {
        kHud = 0,     // "orbitron" role (ChakraPetch substitution ADR)
        kMonoton = 1, // logos; uppercase only
        kRighteous = 2,
        kFontCount = 3
    };

    static constexpr uint32_t kBakedSizes[3] = {24, 48, 96};
    static constexpr uint32_t kAtlasSize = 2048;

    struct Glyph {
        float x0, y0, x1, y1; // atlas UVs
        float xoff, yoff;     // bearing (px, from baseline-left)
        float advance;        // px
    };

    // Bakes from assets/fonts/*.ttf. Returns false (with the error
    // logged) on any IO or pack failure — the caller treats missing
    // text as fatal-per-spec only when art actually uses text.
    bool bake(const std::filesystem::path& fonts_dir);

    // Glyph lookup for (font, baked-size, codepoint); nullptr when the
    // codepoint is not in the atlas (Monoton's lowercase, per §5.1).
    // codepoint is a UNICODE SCALAR VALUE in U+0020-U+00FF (the
    // atlas'"'"'s exact coverage; Latin-1 == byte identity).
    const Glyph* glyph(Font font, uint32_t baked_size, uint32_t codepoint) const;

    // The baked pixels (R8, kAtlasSize²) — uploaded by the renderer.
    const uint8_t* pixels() const { return pixels_.data(); }

    uint32_t pixel_count() const { return uint32_t(pixels_.size()); }

    // Size selection (§14.1): smallest baked size ≥ target pixel
    // height; upscaling beyond 1.15× is forbidden — take the next size
    // up. Returns 0 when even 96 px is below the target.
    static uint32_t select_size(float target_px);

    // Advances in px at a baked size (letter_spacing em applies).
    float text_width(Font font,
                     uint32_t baked_size,
                     const std::string& text,
                     float letter_spacing_em) const;

    bool valid() const { return baked_; }

private:
    bool baked_ = false;
    std::vector<uint8_t> pixels_;
    // glyphs[font][sizeIdx][cp - 0x20] (Latin-1sup range compacted to
    // 0xA0-0xFF appended after ASCII).
    Glyph glyphs_[kFontCount][3][224] = {};
    bool has_[kFontCount][3][224] = {};
};

} // namespace tb::render
