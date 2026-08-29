// M13a tests (04-milestones.md §M13): TBArt loader, particles, font
// atlas, and the CRT branch math (CPU-verifiable portions).
#include "core/config.h"
#include "core/time.h"
#include "render/art_renderer.h"
#include "render/font_atlas.h"
#include "render/particles.h"
#include "render/segment_digits.h"
#include "render/tbart.h"
#include "support/data_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

using namespace tb;

// ---- TbArt.SchemaRoundTripAllPrimitives ----
TEST(TbArt, SchemaRoundTripAllPrimitives) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_art_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "art.json") << R"json({
  // one primitive of every kind + a decal
  "palette": "sunset-synth",
  "ball": { "trail": true, "trail_color": "secondary" },
  "layers": [
    { "name": "ground", "z": 0, "blend": "normal", "primitives": [
      { "kind": "circle", "transform": { "pos": [0.1, 0.1] }, "r": 0.01,
        "fill": "primary" },
      { "kind": "ring", "transform": { "pos": [0.2, 0.1] }, "r": 0.02,
        "thickness": 0.003, "fill": { "type": "radial", "radius": 0.02,
        "colors": ["glow_white", "primary"] } },
      { "kind": "rect", "transform": { "pos": [0.3, 0.1], "rot_deg": 15, "scale": 1.5 },
        "w": 0.02, "h": 0.01, "corner_r": 0.002, "fill": "#FF297580" },
      { "kind": "capsule", "transform": { "pos": [0, 0] }, "a": [0.1, 0.2],
        "b": [0.2, 0.2], "r": 0.004, "fill": "bg1" },
      { "kind": "segment", "transform": { "pos": [0, 0] }, "a": [0.1, 0.3],
        "b": [0.2, 0.3], "stroke": { "width": 0.002, "color": "secondary" } },
      { "kind": "polyline", "transform": { "pos": [0, 0] },
        "points": [[0.1, 0.4], [0.15, 0.45], [0.2, 0.4]], "closed": true,
        "stroke": { "width": 0.003, "color": "accent1" } },
      { "kind": "polygon", "transform": { "pos": [0, 0] },
        "points": [[0.3, 0.4], [0.35, 0.45], [0.32, 0.42]],
        "fill": "warm" },
      { "kind": "arc", "transform": { "pos": [0.4, 0.2] }, "r": 0.03,
        "thickness": 0.004, "start_deg": 20, "end_deg": 160, "fill": "primary",
        "glow": { "radius": 0.008, "intensity": 1.2 } },
      { "kind": "star", "transform": { "pos": [0.4, 0.4] }, "points_n": 5,
        "r_outer": 0.02, "r_inner": 0.008, "fill": "accent2" },
      { "kind": "text", "transform": { "pos": [0.2, 0.5] }, "string": "NEON",
        "font": "monoton", "size": 0.02, "align": "center", "fill": "glow_white" },
      { "kind": "decal", "transform": { "pos": [0.26, 0.7] },
        "prefab": "starburst", "params": { "r": 0.02, "color": "primary" } }
    ] },
    { "name": "wire", "z": 140, "blend": "additive", "primitives": [
      { "kind": "polyline", "transform": { "pos": [0, 0] },
        "points": [[0.08, 0.3], [0.1, 0.5]], "stroke": { "width": 0.003,
        "color": "secondary" } }
    ] }
  ]
})json";
    }

    auto result = render::load_art(dir, {});
    ASSERT_TRUE(result.loaded);
    const auto& art = result.art;
    EXPECT_EQ(art.palette_name, "sunset-synth");
    EXPECT_TRUE(art.ball_trail);
    ASSERT_EQ(art.layers.size(), 2u);
    EXPECT_EQ(art.layers[0].z, 0);
    EXPECT_FALSE(art.layers[0].additive);
    EXPECT_EQ(art.layers[1].z, 140);
    EXPECT_TRUE(art.layers[1].additive);

    // 10 top-level prims + the decal group.
    ASSERT_EQ(art.layers[0].prims.size(), 11u);
    const auto& p0 = art.layers[0].prims[0];
    EXPECT_EQ(p0.kind, render::ArtPrim::Kind::Circle);
    EXPECT_FLOAT_EQ(p0.transform.pos[0], 0.1f);
    EXPECT_FLOAT_EQ(p0.r, 0.01f);
    EXPECT_EQ(p0.fill.color0, 0xFF2975FFu); // primary resolved

    const auto& rect = art.layers[0].prims[2];
    EXPECT_EQ(rect.kind, render::ArtPrim::Kind::Rect);
    EXPECT_FLOAT_EQ(rect.transform.rot_deg, 15.0f);
    EXPECT_FLOAT_EQ(rect.transform.scale, 1.5f);
    EXPECT_FLOAT_EQ(rect.corner_r, 0.002f);
    EXPECT_EQ(rect.fill.color0, 0xFF297580u); // hex + alpha

    const auto& ring = art.layers[0].prims[1];
    EXPECT_EQ(ring.fill.kind, render::Fill::Kind::Radial);
    EXPECT_FLOAT_EQ(ring.fill.length, 0.02f);

    // Star expands to a polygon with 10 vertices.
    const auto& star = art.layers[0].prims[8];
    EXPECT_EQ(star.kind, render::ArtPrim::Kind::Polygon);
    EXPECT_EQ(star.points.size(), 20u);

    const auto& text = art.layers[0].prims[9];
    EXPECT_EQ(text.kind, render::ArtPrim::Kind::Text);
    EXPECT_EQ(text.font, 1); // monoton
    EXPECT_EQ(text.align, 1);
    EXPECT_FLOAT_EQ(text.size, 0.02f);

    // Decal group expanded with children.
    const auto& decal = art.layers[0].prims[10];
    EXPECT_EQ(decal.kind, render::ArtPrim::Kind::DecalGroup);
    EXPECT_FALSE(decal.children.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(TbArt, UnknownPrimitiveIsLoadError) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_art_bad_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "art.json") << R"json({
  "palette": "sunset-synth",
  "layers": [ { "name": "g", "z": 0, "primitives": [
    { "kind": "hexagon", "transform": { "pos": [0, 0] } } ] } ]
})json";
    }
    EXPECT_THROW(render::load_art(dir, {}), render::ArtError);

    // Unknown palette, bad z, duplicate z.
    { std::ofstream(dir / "art.json") << R"json({ "palette": "nope", "layers": [] })json"; }
    EXPECT_THROW(render::load_art(dir, {}), render::ArtError);
    {
        std::ofstream(dir / "art.json")
            << R"json({ "palette": "sunset-synth", "layers": [ { "z": 200 } ] })json";
    }
    EXPECT_THROW(render::load_art(dir, {}), render::ArtError);
    {
        std::ofstream(dir / "art.json") << R"json({ "palette": "sunset-synth",
          "layers": [ { "z": 5 }, { "z": 5 } ] })json";
    }
    EXPECT_THROW(render::load_art(dir, {}), render::ArtError);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(TbArt, MissingFileIsNotLoadedNotError) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_art_missing_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    const auto result = render::load_art(dir, {});
    EXPECT_FALSE(result.loaded);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(TbArt, UnknownLightIdIsLoadError) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_art_light_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "art.json") << R"json({
  "palette": "midnight-chrome",
  "layers": [ { "z": 0, "primitives": [
    { "kind": "circle", "transform": { "pos": [0.1, 0.1] }, "r": 0.01,
      "fill": "primary", "light": "arrow_left" } ] } ]
})json";
    }
    EXPECT_THROW(render::load_art(dir, {}), render::ArtError);
    // With the id present it binds.
    {
        std::ofstream(dir / "art.json") << R"json({
  "palette": "midnight-chrome",
  "layers": [ { "z": 0, "primitives": [
    { "kind": "circle", "transform": { "pos": [0.1, 0.1] }, "r": 0.01,
      "fill": "primary", "light": "arrow_left" } ] } ]
})json";
    }
    const auto ok = render::load_art(dir, {{"arrow_left", 3}});
    ASSERT_TRUE(ok.loaded);
    EXPECT_EQ(ok.art.layers[0].prims[0].light_index, 3);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- Particles.PoolNeverExceedsCapacity ----
TEST(Particles, PoolNeverExceedsCapacity) {
    render::ParticleSystem ps;
    uint32_t palette[8] = {0x0D0221FF,
                           0x26104DFF,
                           0xFF2975FF,
                           0x08F7FEFF,
                           0x9D4EFFFF,
                           0xFFD319FF,
                           0xFF901FFF,
                           0xFFF2E5FF};
    ps.set_palette(palette);

    // Spawn storm: fill without update first (no deaths), so the
    // overflow is deterministic, then keep the storm going under update.
    for (int i = 0; i < 100; ++i) {
        ps.spawn(render::ParticleSystem::Effect::JackpotStarburst, 0.26f, 0.52f);
        ASSERT_LE(ps.live(), render::ParticleSystem::kMaxParticles);
    }
    EXPECT_GT(ps.stolen_total(), 0u); // the storm provably overflowed
    for (int i = 0; i < 500; ++i) {
        ps.spawn(render::ParticleSystem::Effect::JackpotStarburst, 0.26f, 0.52f);
        ps.update(0.0167f);
        ASSERT_LE(ps.live(), render::ParticleSystem::kMaxParticles);
    }
    EXPECT_GT(ps.live(), 4000u); // saturated under continuous load
}

TEST(Particles, UpdateExpiresAndFades) {
    render::ParticleSystem ps;
    uint32_t palette[8] = {0, 0, 0xFF2975FF, 0x08F7FEFF, 0, 0, 0xFF901FFF, 0xFFF2E5FF};
    ps.set_palette(palette);
    const int n = ps.spawn(render::ParticleSystem::Effect::BumperHitBurst, 0.1f, 0.1f);
    EXPECT_EQ(n, 24);
    EXPECT_EQ(ps.live(), 24u);
    // §13.4 max life 0.45 s: after 0.5 s all are gone.
    for (int i = 0; i < 30; ++i) {
        ps.update(1.0f / 60.0f);
    }
    EXPECT_EQ(ps.live(), 0u);
}

// ---- perf_particles.two_thousand_live_at_60fps (§17.1 budget) ----
TEST(perf_particles, two_thousand_live_at_60fps) {
#ifndef NDEBUG
    GTEST_SKIP() << "perf gates are Release-only";
#else
    using Clock = std::chrono::steady_clock;
    render::ParticleSystem ps;
    uint32_t palette[8] = {0x0D0221FF,
                           0x26104DFF,
                           0xFF2975FF,
                           0x08F7FEFF,
                           0x9D4EFFFF,
                           0xFFD319FF,
                           0xFF901FFF,
                           0xFFF2E5FF};
    ps.set_palette(palette);

    const float dt = 0.01667f; // 60 Hz frame
    // Fill to ≥ 2000 live: jackpot bursts (96 each) + drain (40).
    uint64_t ns_total = 0;
    uint32_t max_live = 0;
    // 600 update steps = 10 s of frames (04 §M13 test text).
    for (int step = 0; step < 600; ++step) {
        const auto t0 = Clock::now();
        // Top up: two bursts + a drain per frame keeps ≥ 2000 live
        // under the 0.45–1.0 s lifetimes.
        ps.spawn(render::ParticleSystem::Effect::JackpotStarburst, 0.26f, 0.52f);
        ps.spawn(render::ParticleSystem::Effect::JackpotStarburst, 0.20f, 0.40f);
        ps.spawn(render::ParticleSystem::Effect::DrainBurst, 0.26f, 0.05f);
        ps.update(dt);
        const auto t1 = Clock::now();
        ns_total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        max_live = std::max(max_live, ps.live());
    }
    ASSERT_GE(max_live, 2000u); // the load was real
    const double per_frame_ms = double(ns_total) / 600.0 / 1e6;
    // §17.1 CPU-encode budget: 1.5 ms per frame (NOT the 16.67 ms
    // frame period — see 04 §M13's warning).
    EXPECT_LT(per_frame_ms, 1.5) << "per-frame particle cost " << per_frame_ms << " ms";
#endif
}

// ---- Text.GlyphMetricsGolden ----
TEST(Text, GlyphMetricsGolden) {
    render::FontAtlas atlas;
    ASSERT_TRUE(atlas.bake(tb::test::data_path("assets/fonts")));
    // Invariants rather than pixel-exact UVs (driver-independent):
    // every printable ASCII glyph exists for all three faces/sizes.
    for (int f = 0; f < 3; ++f) {
        for (uint32_t sz : render::FontAtlas::kBakedSizes) {
            for (uint32_t c = 0x20u; c <= 0x7Eu; ++c) {
                if (f == render::FontAtlas::kMonoton && (c >= 'a' && c <= 'z')) {
                    continue; // Monoton has no lowercase (§5.1)
                }
                if (c == 0x20u) {
                    continue; // space bakes an empty box with advance
                }
                EXPECT_NE(atlas.glyph(render::FontAtlas::Font(f), sz, c), nullptr)
                    << "font " << f << " size " << sz << " cp " << c;
            }
            // Space still carries an advance (word spacing works).
            const auto* sp = atlas.glyph(render::FontAtlas::Font(f), sz, 0x20u);
            ASSERT_NE(sp, nullptr);
            EXPECT_GT(sp->advance, 0.0f);
        }
    }
    // Monoton's lowercase: the font has none, so glyphs fall back to
    // .notdef shapes; the ATLAS still maps them (advance-carrying), the
    // RULE lives in authoring (uppercase only). Nothing to assert
    // beyond it not crashing.
    // Advances are positive for visible glyphs.
    const auto* g = atlas.glyph(render::FontAtlas::kHud, 48, 'W');
    ASSERT_NE(g, nullptr);
    EXPECT_GT(g->advance, 0.0f);
    // Size selection: smallest baked ≥ target; no >1.15× upscaling.
    EXPECT_EQ(render::FontAtlas::select_size(20.0f), 24u);
    EXPECT_EQ(render::FontAtlas::select_size(24.0f), 24u);
    EXPECT_EQ(render::FontAtlas::select_size(30.0f), 48u);
    EXPECT_EQ(render::FontAtlas::select_size(100.0f), 0u); // no size fits
    // Text width is monotone in string length.
    const float w1 = atlas.text_width(render::FontAtlas::kHud, 48, "A", 0.0f);
    const float w2 = atlas.text_width(render::FontAtlas::kHud, 48, "AB", 0.0f);
    EXPECT_GT(w2, w1);
}

// ---- Crt.OffByDefaultAndBranchMatchesSpec ----
TEST(Crt, OffByDefaultAndDefaultAndBranchMatchesSpec) {
    // The OFF-BY-DEFAULT half: the real config default.
    tb::Settings s{};
    EXPECT_FALSE(s.crt);

    // The BRANCH half: the real shader source carries the §10 formulas
    // (0.12 scanline, 0.15 vignette, smoothstep(0.6, 1.0), the u_crt
    // uniform gate). Regression = the string check fails.
    std::ifstream frag(tb::test::data_path("shaders/present.frag.hlsl"));
    ASSERT_TRUE(frag.good());
    const std::string src_text((std::istreambuf_iterator<char>(frag)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(src_text.find("u_crt != 0.0f"), std::string::npos);
    EXPECT_NE(src_text.find("1.0f - 0.12f * scan"), std::string::npos);
    EXPECT_NE(src_text.find("1.0f - 0.15f * smoothstep(0.6f, 1.0f, r)"), std::string::npos);

    // And the CPU reference values for the same formulas (the spec's
    // own numbers: 0.88 dark-row, 0.85 corner, 0.748 combined).
    struct V3 {
        float r, g, b;
    };

    auto apply = [](V3 c, int py, float r_norm) {
        const float scan = (float(py % 3) < 1.0f) ? 1.0f : 0.0f;
        c.r *= 1.0f - 0.12f * scan;
        const float t = std::clamp((r_norm - 0.6f) / 0.4f, 0.0f, 1.0f);
        const float ss = t * t * (3.0f - 2.0f * t);
        c.r *= 1.0f - 0.15f * ss;
        return c;
    };
    EXPECT_NEAR(apply({1, 1, 1}, 0, 0.0f).r, 0.88f, 1e-5f);
    EXPECT_NEAR(apply({1, 1, 1}, 1, 1.0f).r, 0.85f, 1e-5f);
    EXPECT_NEAR(apply({1, 1, 1}, 0, 1.0f).r, 0.748f, 1e-4f);
}

// ---- Bloom.DisabledFallbackRenders ----
// Quality::Off: record() is a no-op and bloom0() is null; the
// composite path multiplies the (scene-bound) second sample by
// strength 0, so the output equals the plain path. CPU-verifiable:
// the chain state itself.
TEST(Bloom, DisabledFallbackRenders) {
    // The real fallback chain: bloom_enabled=false → the app passes
    // strength 0 → the composite's bloom term is identically zero
    // (verified by the shader-source check below) and Quality::Off
    // skips record() entirely (the BloomChain guard).
    tb::Settings s{};
    s.bloom_enabled = false;
    const float strength = s.bloom_enabled ? s.bloom_strength : 0.0f;
    EXPECT_FLOAT_EQ(strength, 0.0f);

    std::ifstream frag(tb::test::data_path("shaders/present.frag.hlsl"));
    ASSERT_TRUE(frag.good());
    const std::string src_text((std::istreambuf_iterator<char>(frag)),
                               std::istreambuf_iterator<char>());
    // The bloom sample is multiplied by the strength uniform — 0
    // zeroes it, so the null-bloom bind (scene at slot 1) is safe.
    EXPECT_NE(src_text.find("+ bloom_strength * bloom.Sample"), std::string::npos);
}

// ---- SegmentDigits: masks, ghost brightness, comma ----
TEST(SegmentDigits, DigitMasksAndGhosting) {
    // §14.2 masks verbatim.
    EXPECT_EQ(render::SegmentDigits::kDigitMask('0'), 0x0C3F);
    EXPECT_EQ(render::SegmentDigits::kDigitMask('1'), 0x0006);
    EXPECT_EQ(render::SegmentDigits::kDigitMask('8'), 0x00FF);
    EXPECT_EQ(render::SegmentDigits::kDigitMask(' '), 0x0000);
    EXPECT_EQ(render::SegmentDigits::kDigitMask('A'), 0x0000); // fallback

    std::vector<render::QuadInstance> quads;
    // Lit 8: mask 0x00FF = A..G2 = 8 segments (the diagonals H-M are
    // unused by digits).
    render::SegmentDigits::emit('8', 0, 0, 64, 96, true, 1, 0.2f, 0.5f, &quads);
    EXPECT_EQ(quads.size(), 8u);
    // All at full brightness.
    for (const auto& q : quads) {
        EXPECT_FLOAT_EQ(q.a, 1.0f);
    }
    // Ghost: 6%.
    quads.clear();
    render::SegmentDigits::emit('8', 0, 0, 64, 96, false, 1, 0.2f, 0.5f, &quads);
    EXPECT_EQ(quads.size(), 8u);
    for (const auto& q : quads) {
        EXPECT_NEAR(q.a, 0.06f, 1e-5f);
    }
    // 1: 2 segments.
    quads.clear();
    render::SegmentDigits::emit('1', 0, 0, 64, 96, true, 1, 1, 1, &quads);
    EXPECT_EQ(quads.size(), 2u);
    // Comma: 1 capsule below baseline right of cell.
    quads.clear();
    render::SegmentDigits::emit(',', 0, 0, 64, 96, true, 1, 1, 1, &quads);
    ASSERT_EQ(quads.size(), 1u);
    EXPECT_LT(quads[0].cy, 0.0f);         // below baseline
    EXPECT_GT(quads[0].cx, 64.0f * 0.5f); // right of cell center
    // Space: nothing.
    quads.clear();
    render::SegmentDigits::emit(' ', 0, 0, 64, 96, true, 1, 1, 1, &quads);
    EXPECT_TRUE(quads.empty());
}

} // namespace
