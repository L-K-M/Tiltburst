// M12 display topology tests (07-displays.md §12, T1–T15 + pacing).
#include "platform/backglass_pacer.h"
#include "platform/display_detect.h"
#include "render/backglass_renderer.h"
#include "render/overlay.h"
#include "sim/script_host.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {

using namespace tb;

platform::DisplayInfo
make_display(int index, int w, int h, float hz = 60.0f, const char* name = "") {
    platform::DisplayInfo d;
    d.index = index;
    d.w = w;
    d.h = h;
    d.refresh_hz = hz;
    d.name = name;
    return d;
}

// ---- T1: reference cabinet — rotated TV + 5:4 backglass ----
TEST(DisplayAssign, PortraitPlusSquare) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1);
    EXPECT_EQ(a.pf_rotation, 90);
    EXPECT_EQ(a.bg_rotation, 0);
    EXPECT_TRUE(a.warnings.empty());
}

// ---- T2: true portrait + 4:3 — no projection rotation ----
TEST(DisplayAssign, TruePortraitNoRotation) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920),
        make_display(1, 1024, 768),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1);
    EXPECT_EQ(a.pf_rotation, 0);
}

// ---- T3 + T5: single landscape / single portrait -> playfield only ----
TEST(DisplayAssign, SingleDisplayPlayfieldOnly) {
    {
        std::vector<platform::DisplayInfo> ds = {make_display(0, 2560, 1440, 144)};
        const auto a = platform::detect(ds, {});
        EXPECT_EQ(a.playfield, 0);
        EXPECT_EQ(a.backglass, -1);
        EXPECT_EQ(a.pf_rotation, 0);
    }
    {
        std::vector<platform::DisplayInfo> ds = {make_display(0, 1080, 1920)};
        const auto a = platform::detect(ds, {});
        EXPECT_EQ(a.playfield, 0);
        EXPECT_EQ(a.backglass, -1);
        EXPECT_EQ(a.pf_rotation, 0);
    }
}

// ---- T4: laptop + external — larger wins, nothing sideways ----
TEST(DisplayAssign, LargerLandscapeWins) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "Laptop"),
        make_display(1, 2560, 1440, 60, "U2719"),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 1);
    EXPECT_EQ(a.backglass, 0);
    EXPECT_EQ(a.pf_rotation, 0); // backglass squareness 0.5625 < 0.70
}

// ---- Landscape-reported-portrait rotates (the T1 shape, named) ----
TEST(DisplayAssign, LandscapeReportedPortraitRotates) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080),
        make_display(1, 1280, 1024),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.pf_rotation, 90); // cabinet assumption
}

// ---- T6: portrait beats larger landscape; squarer wins backglass ----
TEST(DisplayAssign, ThreeDisplaysPicksSquarest) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920),
        make_display(1, 1280, 1024),
        make_display(2, 1920, 1080),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1); // squareness 0.80 > 0.5625
    EXPECT_EQ(a.pf_rotation, 0);
}

TEST(DisplayAssign, LargerPortraitWinsBetweenPortraits) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920),
        make_display(1, 1440, 2560),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 1);
    EXPECT_EQ(a.backglass, 0);
}

// ---- T8: config beats heuristics ----
TEST(DisplayAssign, OverrideBeatsHeuristic) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "index:1";
    cfg.backglass.match = "index:0";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 1);
    EXPECT_EQ(a.backglass, 0);
    EXPECT_EQ(a.pf_rotation, 0); // backglass 1920×1080 squareness 0.5625 < 0.70
}

// ---- T9: stale name falls back with the warning ----
TEST(DisplayAssign, StaleMatchWarnsAndFallsBack) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "name:LG*";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0); // heuristic result = T1
    EXPECT_EQ(a.pf_rotation, 90);
    ASSERT_EQ(a.warnings.size(), 1u);
    EXPECT_NE(a.warnings[0].find("not found"), std::string::npos);
}

// ---- T10: identical names — deterministic ties ----
TEST(DisplayAssign, IdenticalDisplaysDeterministicTie) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "Same"),
        make_display(1, 1920, 1080, 60, "Same"),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1);
    EXPECT_EQ(a.pf_rotation, 0); // bg squareness 0.5625 < 0.70
}

// ---- T11: empty list ----
TEST(DisplayAssign, EmptyListNoAssignment) {
    const auto a = platform::detect({}, {});
    EXPECT_EQ(a.playfield, -1);
    EXPECT_EQ(a.backglass, -1);
}

// ---- T12: explicit rotation passthrough ----
TEST(DisplayAssign, ExplicitRotationPassthrough) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.rotation = "270";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.pf_rotation, 270);
    EXPECT_EQ(a.backglass, 1);
}

// ---- T13: backglass disabled -> no cabinet rotation ----
TEST(DisplayAssign, BackglassDisabledNoRotation) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.backglass.enabled = false;
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, -1);
    EXPECT_EQ(a.pf_rotation, 0); // no backglass -> desktop assumption
}

// ---- T14: last_auto stability path ----
TEST(DisplayAssign, LastAutoStability) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.last_auto.present = true;
    cfg.last_auto.playfield = "TV";
    cfg.last_auto.backglass = "NEC";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1);
    EXPECT_EQ(a.pf_rotation, 90);    // rotations still resolve via step 4
    EXPECT_TRUE(a.stability_reused); // probe flag
}

TEST(DisplayAssign, LastAutoStaleFallsBackToHeuristic) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.last_auto.present = true;
    cfg.last_auto.playfield = "OLD-TV";
    cfg.last_auto.backglass = "NEC";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0); // heuristic
    EXPECT_FALSE(a.stability_reused);
}

// ---- T15: refresh-rate tiebreak ----
TEST(DisplayAssign, RefreshRateTiebreak) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60),
        make_display(1, 1920, 1080, 144),
        make_display(2, 1280, 1024, 60, "NEC"),
    };
    const auto a = platform::detect(ds, {});
    EXPECT_EQ(a.playfield, 1); // equal area, higher Hz
    EXPECT_EQ(a.backglass, 2); // the NEC: squareness 0.80 beats 0.5625
    EXPECT_EQ(a.pf_rotation, 90); // the NEC backglass: cabinet (0.80 >= 0.70)
}

// ---- name glob specifics ----
TEST(DisplayAssign, NameGlobMatching) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "SAMSUNG 32in TV"),
        make_display(1, 1280, 1024, 60, "NEC 1280"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "name:NEC*";
    cfg.backglass.match = "name:*TV";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 1);
    EXPECT_EQ(a.backglass, 0);
}

TEST(DisplayAssign, AmbiguousNameGlobLowestIndex) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "AOC"),
        make_display(1, 1280, 1024, 60, "AOC"),
    };
    bool ambiguous = false;
    const int idx = platform::resolve_match("name:AOC", ds, &ambiguous);
    EXPECT_EQ(idx, 0);
    EXPECT_TRUE(ambiguous);
}

TEST(DisplayAssign, SameDisplayForBothRolesDropsBackglass) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "index:0";
    cfg.backglass.match = "index:0";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, -1);
    EXPECT_EQ(a.warnings.size(), 1u);
}

// ---- displays.json round-trip (07 §5) ----
TEST(DisplaysJson, PersistRoundTrip) {
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "name:LG*";
    cfg.playfield.rotation = "270";
    cfg.backglass.enabled = false;
    cfg.last_auto.present = true;
    cfg.last_auto.playfield = "TV";
    cfg.last_auto.backglass = "NEC 1280";

    const std::string text = platform::save_displays_json(cfg);
    const auto res = platform::load_displays_json(text);
    ASSERT_TRUE(res.loaded);
    EXPECT_EQ(res.cfg.playfield.match, "name:LG*");
    EXPECT_EQ(res.cfg.playfield.rotation, "270");
    EXPECT_FALSE(res.cfg.backglass.enabled);
    ASSERT_TRUE(res.cfg.last_auto.present);
    EXPECT_EQ(res.cfg.last_auto.playfield, "TV");
    EXPECT_EQ(res.cfg.last_auto.backglass, "NEC 1280");

    // Comments allowed (canon §5.5), corrupt -> defaults.
    EXPECT_TRUE(
        platform::load_displays_json("// comment\n{\"playfield\":{\"match\":\"index:1\"}}").loaded);
    const auto bad = platform::load_displays_json("{ not json ]");
    EXPECT_FALSE(bad.loaded);
    EXPECT_TRUE(bad.corrupt);
    // Missing file: empty text.
    EXPECT_FALSE(platform::load_displays_json("").loaded);
}

// ---- BackglassPacing: skip-never-blocks semantics (07 §8) ----
TEST(BackglassPacing, SkipNeverBlocks) {
    platform::BackglassPacer pacer;
    const uint64_t t0 = 1'000'000'000ull;

    // First frame: due immediately.
    ASSERT_TRUE(pacer.should_attempt(t0));
    pacer.report_drawn(t0);
    EXPECT_EQ(pacer.drawn_frames(), 1u);
    EXPECT_EQ(pacer.skips(), 0u);

    // Not due until +33.33 ms.
    EXPECT_FALSE(pacer.should_attempt(t0 + 16'000'000));
    EXPECT_TRUE(pacer.should_attempt(t0 + 33'333'333));

    // A skipped acquire retries next playfield frame WITHOUT advancing
    // the deadline.
    pacer.report_skipped();
    EXPECT_EQ(pacer.skips(), 1u);
    EXPECT_TRUE(pacer.should_attempt(t0 + 34'000'000)); // retry immediately
    pacer.report_drawn(t0 + 67'000'000);                // late but drawn
    EXPECT_EQ(pacer.drawn_frames(), 2u);

    // Hitch resync: > 100 ms behind snaps the deadline.
    const uint64_t t_after_hitch = t0 + 500'000'000;
    EXPECT_TRUE(pacer.should_attempt(t_after_hitch));
    pacer.report_drawn(t_after_hitch);
    // Next attempt is a frame later, not half a second late.
    EXPECT_FALSE(pacer.should_attempt(t_after_hitch + 10'000'000));
    EXPECT_TRUE(pacer.should_attempt(t_after_hitch + 33'333'333));
}

// ---- BackglassLayout: content reaches the draw list ----
TEST(BackglassLayout, BuildsContentForBothModes) {
    render::Overlay font;
    render::BackglassLayout layout;
    std::vector<render::QuadInstance> quads;

    render::BackglassContent attract;
    attract.in_attract = true;
    attract.high_score_count = 2;
    attract.high_scores[0] = {{'T', 'B', 'T'}, 1234567};
    attract.high_scores[1] = {{'A', 'X', 'L'}, 999};
    sim::BackglassModel model;
    layout.build(attract, model, font, &quads);
    EXPECT_GT(quads.size(), 4u); // bg + title + 2 rows
    // Attract layout is bounds-checked too (cycle-18 review).
    for (const auto& q : quads) {
        EXPECT_GE(q.cx - q.hx, -1.0f);
        EXPECT_LE(q.cx + q.hx, float(render::BackglassFrame::kCanvasW) + 1.0f);
        EXPECT_GE(q.cy - q.hy, -1.0f);
        EXPECT_LE(q.cy + q.hy, float(render::BackglassFrame::kCanvasH) + 1.0f);
    }

    quads.clear();
    render::BackglassContent play;
    play.in_attract = false;
    play.player_count = 4;
    play.current_player = 2;
    play.ball_number = 3;
    for (int i = 0; i < 4; ++i) {
        play.scores[size_t(i)] = uint64_t(i + 1) * 1000000;
    }
    model.message_len = 4;
    std::memcpy(model.message, "TEST", 4);
    model.message_style = 2; // jackpot
    layout.build(play, model, font, &quads);
    // Background + 4 cards + band + message: well over a dozen quads
    // (each text line contributes glyph quads).
    EXPECT_GT(quads.size(), 10u);

    // Every quad lands inside the canvas.
    for (const auto& q : quads) {
        EXPECT_GE(q.cx - q.hx, -1.0f);
        EXPECT_LE(q.cx + q.hx, float(render::BackglassFrame::kCanvasW) + 1.0f);
        EXPECT_GE(q.cy - q.hy, -1.0f);
        EXPECT_LE(q.cy + q.hy, float(render::BackglassFrame::kCanvasH) + 1.0f);
    }
}

// ---- Cycle-1 regressions ----

// A failed explicit backglass match falls back to the heuristic (the
// warning promises it).
TEST(DisplayAssign, FailedBackglassMatchFallsBackToHeuristic) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "index:0";
    cfg.backglass.match = "name:MISSING*";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1); // heuristic picked the NEC
    EXPECT_EQ(a.pf_rotation, 90);
}

// last_auto with an EMPTY backglass must not block discovery when a
// second display appears (set equality, both directions).
TEST(DisplayAssign, LastAutoNoBackglassDoesNotBlockDiscovery) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"), // newly attached
    };
    platform::DisplaysConfig cfg;
    cfg.last_auto.present = true;
    cfg.last_auto.playfield = "TV";
    cfg.last_auto.backglass = ""; // previous run: no backglass
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, 1); // the new display is discovered
    EXPECT_FALSE(a.stability_reused);
}

// Malformed index forms never silently bind display 0.
TEST(DisplayAssign, MalformedIndexMatchRejected) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920),
        make_display(1, 1280, 1024),
    };
    EXPECT_EQ(platform::resolve_match("index:abc", ds), -1);
    EXPECT_EQ(platform::resolve_match("index: 1", ds), -1);
    EXPECT_EQ(platform::resolve_match("index:", ds), -1);
    EXPECT_EQ(platform::resolve_match("index:1x", ds), -1);
    EXPECT_EQ(platform::resolve_match("index:1", ds), 1);
}

// An unknown schema version refuses to parse.
TEST(DisplaysJson, UnknownVersionRefused) {
    const auto res = platform::load_displays_json(R"json({"version": 2})json");
    EXPECT_FALSE(res.loaded);
    EXPECT_TRUE(res.corrupt);
}

// Ambiguous name matches surface as warnings.
TEST(DisplayAssign, AmbiguousGlobWarns) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1080, 1920, 60, "AOC"),
        make_display(1, 1280, 1024, 60, "AOC"),
    };
    platform::DisplaysConfig cfg;
    cfg.playfield.match = "name:AOC*";
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    ASSERT_EQ(a.warnings.size(), 1u);
    EXPECT_NE(a.warnings[0].find("ambiguous"), std::string::npos);
}

// A disabled backglass must stay disabled through the stability path.
TEST(DisplayAssign, DisabledBackglassSurvivesStabilityReuse) {
    std::vector<platform::DisplayInfo> ds = {
        make_display(0, 1920, 1080, 60, "TV"),
        make_display(1, 1280, 1024, 60, "NEC"),
    };
    platform::DisplaysConfig cfg;
    cfg.backglass.enabled = false;
    cfg.last_auto.present = true;
    cfg.last_auto.playfield = "TV";
    cfg.last_auto.backglass = "NEC"; // last run had one; user disabled it
    const auto a = platform::detect(ds, cfg);
    EXPECT_EQ(a.playfield, 0);
    EXPECT_EQ(a.backglass, -1); // disabled, not resurrected
    // The PLAYFIELD half of the reuse still ran: disabled backglass
    // must not cost playfield stability (cycle-4).
    EXPECT_TRUE(a.stability_reused);
}

// A string/float version is an unknown schema, refused.
TEST(DisplaysJson, NonIntegerVersionRefused) {
    EXPECT_TRUE(platform::load_displays_json(R"json({"version": "2"})json").corrupt);
    EXPECT_TRUE(platform::load_displays_json(R"json({"version": 2.5})json").corrupt);
}

// Overflowing index never UBs.
TEST(DisplayAssign, OverflowIndexRejected) {
    std::vector<platform::DisplayInfo> ds = {make_display(0, 1080, 1920)};
    EXPECT_EQ(platform::resolve_match("index:99999999999", ds), -1);
    EXPECT_EQ(platform::resolve_match("index:2147483648", ds), -1);
}

} // namespace
