#include "sim/solver.h"
#include "support/data_path.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

// M5 table-format tests (04-milestones.md §M5): loader, prefab expansion,
// plunger curve, and the greybox determinism replay.
namespace {

std::filesystem::path table_dir(const char* slug) {
    return tb::test::data_path(std::string("tables/") + slug);
}

} // namespace

TEST(TableLoader, TestLabParses) {
    const tb::table::TableDef def = tb::table::load_table(table_dir("test-lab"));
    EXPECT_EQ(def.slug, "test-lab");
    EXPECT_EQ(def.name, "Test Lab");
    EXPECT_FLOAT_EQ(def.width, 0.52f);
    EXPECT_FLOAT_EQ(def.height, 1.04f);
    EXPECT_EQ(def.ball_count, 4);

    // 11 declared elements + flippers(2) + shooter(4 standalone — test-lab
    // has no orbit, so the top post survives) + slings(4) = 21.
    ASSERT_EQ(def.elements.size(), 21u);

    bool found = false;
    for (const auto& e : def.elements) {
        if (e.id() == "shooter_plunger") {
            found = true;
            const auto& p = std::get<tb::table::PlungerDef>(e.def);
            EXPECT_FLOAT_EQ(p.pos[0], 0.500f);
            EXPECT_FLOAT_EQ(p.pos[1], 0.030f);
        }
    }
    EXPECT_TRUE(found) << "plunger_lane prefab did not expand";
}

TEST(TableLoader, NeonDriftParsesMergedLane) {
    const tb::table::TableDef def = tb::table::load_table(table_dir("neon-drift"));
    // The orbit's right mouth [0.445, 0.900] is within 0.045 m of the
    // shooter wall top [0.480, 0.888] (0.037): merged variant, no top post.
    bool has_top_post = false;
    for (const auto& e : def.elements) {
        if (e.id() == "shooter_top_post") {
            has_top_post = true;
        }
    }
    EXPECT_FALSE(has_top_post) << "merged lane must not emit a top post (09 §5.2)";
}

TEST(TableLoader, EveryM5ElementRoundTrips) {
    // A table exercising every M5 element type + all four prefabs, written
    // to a temp dir and loaded back.
    const std::string json = R"JSON({
      "format_version": 1,
      "meta": { "slug": "rt", "name": "RoundTrip", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "playfield": { "size": [0.52, 1.04], "slope_deg": 6.5, "ball_count": 3 },
      "physics": { "rolling_resistance": 0.03 },
      "materials": { "rubber": { "restitution": 0.7 } },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true, "material": "wood",
          "path": [[0.0, 0.0], [0.0, 0.94],
                   { "arc": { "to": [0.10, 1.04], "radius": 0.10, "dir": "cw" } },
                   [0.42, 1.04],
                   { "arc": { "to": [0.52, 0.94], "radius": 0.10, "dir": "cw" } },
                   [0.52, 0.0]] },
        { "id": "guide", "type": "wall", "material": "steel",
          "path": [[0.1, 0.4], [0.2, 0.3]] },
        { "id": "pin", "type": "post", "pos": [0.26, 0.5], "radius": 0.006 },
        { "id": "flip", "type": "flipper", "pos": [0.2, 0.2],
          "rest_angle_deg": -31, "side": "left", "input": "upper_left",
          "strength": 0.9 },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 3 },
        { "id": "lamp", "type": "light", "pos": [0.26, 0.6], "shape": "arrow",
          "color": "#112233", "direction_deg": 45 }
      ],
      "prefabs": [
        { "id": "fp", "prefab": "flipper_pair_standard", "pos": [0.24, 0.115],
          "tip_gap": 0.07 },
        { "id": "sh", "prefab": "plunger_lane" },
        { "id": "il_left", "prefab": "inlane_outlane_pair", "side": "left" },
        { "id": "il", "prefab": "inlane_outlane_pair", "side": "right" },
        { "id": "ob", "prefab": "orbit" }
      ]
    })JSON";

    std::filesystem::path dir;
    {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("tb_rt_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir / "table.json") << json;
    }
    const tb::table::TableDef def = tb::table::load_table(dir);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    EXPECT_EQ(def.physics.rolling_resistance, 0.03f);
    bool rubber_found = false;
    bool found_flip = false;
    bool found_lamp = false;
    bool found_il = false;
    bool found_il_left = false;
    for (const auto& e : def.elements) {
        if (e.id() == "pin") {
            const auto& post = std::get<tb::table::PostDef>(e.def);
            EXPECT_FLOAT_EQ(post.radius, 0.006f);
            EXPECT_TRUE(post.material == tb::table::MaterialId::Rubber);
        }
        if (e.id() == "flip") {
            found_flip = true;
            const auto& f = std::get<tb::table::FlipperDef>(e.def);
            EXPECT_FLOAT_EQ(f.rest_angle_deg, -31.0f);
            EXPECT_TRUE(f.left_side);
            EXPECT_EQ(f.input, "upper_left");
        }
        if (e.id() == "lamp") {
            found_lamp = true;
            const auto& l = std::get<tb::table::LightDef>(e.def);
            EXPECT_EQ(l.shape, "arrow");
            EXPECT_FLOAT_EQ(l.direction_deg, 45.0f);
        }
        if (e.id() == "il_side_wall") {
            found_il = true;
            // Right side mirrors x about 0.240: side wall x = 2*0.240 - 0.
            const auto& w = std::get<tb::table::WallDef>(e.def);
            EXPECT_FLOAT_EQ(w.path[0].point[0], 0.48f);
        }
        if (e.id() == "il_left_side_wall") {
            // Left side is the identity: authored x passes through (09 §5.4).
            found_il_left = true;
            const auto& w = std::get<tb::table::WallDef>(e.def);
            EXPECT_FLOAT_EQ(w.path[0].point[0], 0.0f);
            EXPECT_FLOAT_EQ(w.path[0].point[1], 0.36f);
        }
    }
    // Materials override round-trips through build_sim.
    tb::sim::SimState sim;
    tb::table::build_sim(def, sim);
    for (const auto& e : def.elements) {
        if (e.id() == "pin") {
            rubber_found = true;
        }
    }
    EXPECT_TRUE(rubber_found);
    EXPECT_TRUE(found_flip) << "flipper 'flip' missing";
    EXPECT_TRUE(found_lamp) << "light 'lamp' missing";
    EXPECT_TRUE(found_il) << "right inlane side wall missing";
    EXPECT_TRUE(found_il_left) << "left inlane side wall missing";
    EXPECT_NEAR(sim.mats[uint8_t(tb::sim::MaterialId::Rubber)].restitution, 0.7f, 1e-6f);
    EXPECT_NEAR(sim.mu_rr, 0.03f, 1e-7f);
    EXPECT_TRUE(sim.has_plunger);
    EXPECT_EQ(sim.trough_balls, 3);
    EXPECT_EQ(sim.flippers.size(), 3u); // hand-placed + prefab pair
}

TEST(TableLoader, BadFieldReportsJsonPointer) {
    const std::string json = R"JSON({
      "format_version": 1,
      "meta": { "slug": "bad", "name": "B", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "a", "type": "post", "pos": [0.1, 0.5] },
        { "id": "b", "type": "post", "pos": [0.2, 0.5] },
        { "id": "c", "type": "flipper", "rest_angle_deg": -31, "side": "left" },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 4 }
      ]
    })JSON";
    // Element 3 is the flipper with a missing `pos` — the error must name
    // its JSON pointer.
    std::filesystem::path dir;
    {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("tb_bad_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir / "table.json") << json;
    }
    bool threw = false;
    try {
        tb::table::load_table(dir);
    } catch (const tb::table::TableLoadError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("/elements/3/pos"), std::string::npos)
            << "error missing JSON pointer, got: " << e.what();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(threw);
}

TEST(TableLoader, UnknownElementTypeIsPathQualified) {
    const std::string json = R"JSON({
      "format_version": 1,
      "meta": { "slug": "u", "name": "U", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "w", "type": "wormhole", "pos": [0.2, 0.5] }
      ]
    })JSON";
    std::filesystem::path dir;
    {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("tb_unk_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir, ec);
        std::ofstream(dir / "table.json") << json;
    }
    bool threw = false;
    try {
        tb::table::load_table(dir);
    } catch (const tb::table::TableLoadError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("/elements/1/type"), std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(threw);
}

// Plunger.PullToSpeedCurveMatchesSpec (04-milestones.md M5; 08 §6.16):
// v_launch = max_speed·(0.2 + 0.8·q), pure in held_ticks, and the
// skill-shot repeatability guarantee (identical holds ⇒ bit-identical
// launches).
TEST(Plunger, PullToSpeedCurveMatchesSpec) {
    tb::sim::SimState s;
    s.width = 0.52f;
    s.height = 1.04f;
    s.has_plunger = true;
    s.plunger.pos = {0.500f, 0.030f};
    s.plunger.lane_dir = {0.0f, 1.0f};
    s.plunger.max_speed = 7.5f;
    s.plunger.charge_ticks = 1500.0f;
    s.grid.build(s.colliders, s.width, s.height);

    const auto launch_speed_for_ticks = [](uint32_t held_ticks) {
        // Direct evaluation of the release formula (mirrors step_regions).
        const float q = std::min(1.0f, float(held_ticks) / 1500.0f);
        return 7.5f * (0.2f + 0.8f * q);
    };

    EXPECT_NEAR(launch_speed_for_ticks(0), 1.5f, 1e-6f);    // tap floor 20 %
    EXPECT_NEAR(launch_speed_for_ticks(150), 2.1f, 1e-6f);  // q=0.10 -> 0.28
    EXPECT_NEAR(launch_speed_for_ticks(750), 4.5f, 1e-6f);  // q=0.50 -> 0.60
    EXPECT_NEAR(launch_speed_for_ticks(1500), 7.5f, 1e-6f); // full pull
    EXPECT_NEAR(launch_speed_for_ticks(3000), 7.5f, 1e-6f); // clamped at 1

    // End-to-end through the solver: hold 700 ticks, release, read the
    // launch velocity off the ball; repeat for bit-identical results.
    const auto run_launch = [&s]() {
        tb::sim::SimState sim; // fresh copy of the base config
        sim.width = s.width;
        sim.height = s.height;
        sim.has_plunger = true;
        sim.plunger = s.plunger;
        // Plunger face (0.03 m steel, ⊥ ĵ at pos — build_sim's bake).
        tb::sim::Collider face{};
        face.kind = tb::sim::Collider::Kind::Segment;
        face.a = {0.485f, 0.030f};
        face.b = {0.515f, 0.030f};
        face.element_id = 0;
        face.material = tb::sim::MaterialId::Steel;
        sim.colliders.push_back(face);
        sim.grid.build(sim.colliders, sim.width, sim.height);
        tb::sim::Solver solver;
        tb::sim::Ball& b = sim.balls[0];
        b.index = 0;
        b.live = true;
        b.mode = tb::sim::BallMode::Free;
        b.pos = {0.500f, 0.0455f}; // in the 0.04 m contact zone
        b.vel = {0.0f, 0.0f};
        b.last_safe_pos = b.pos;

        tb::sim::TickInput hold;
        hold.buttons = 1u << 4; // plunger action bit (05 §9.1)
        for (int i = 0; i < 700; ++i) {
            solver.step(sim, hold);
        }
        tb::sim::TickInput release;
        release.buttons = 0;
        solver.step(sim, release);
        return b.vel;
    };
    const tb::sim::Vec2 first = run_launch();
    const tb::sim::Vec2 second = run_launch();
    EXPECT_EQ(first.x, second.x);
    EXPECT_EQ(first.y, second.y);
    EXPECT_NEAR(first.y, launch_speed_for_ticks(700), 0.05f); // q=0.4667
    EXPECT_NEAR(first.y, 4.3f, 0.05f);
}

// Determinism.NeonDriftGreyboxReplay: 60 s of deterministic input on the
// greybox must hash identically across two in-process runs (the committed
// per-OS golden arrives with the tape machinery this milestone exercises;
// in-process stability is the milestone's stated contract).
TEST(Determinism, NeonDriftGreyboxReplay) {
    const auto run_greybox = []() {
        const tb::table::TableDef def = tb::table::load_table(table_dir("neon-drift"));
        tb::sim::SimState sim;
        tb::table::build_sim(def, sim);
        // Serve the first ball (the basic M5 loop does this itself, but the
        // replay drives it deterministically from tick 0).
        tb::sim::Ball& b = sim.balls[0];
        b.index = 0;
        b.live = true;
        b.mode = tb::sim::BallMode::Free;
        b.pos = sim.plunger.pos + sim.plunger.lane_dir * (tb::sim::kBallRadius + 0.002f);
        b.vel = {0.0f, 0.0f};
        b.last_safe_pos = b.pos;
        sim.trough_balls = 3;

        tb::sim::Solver solver;
        std::vector<uint64_t> hashes;
        for (int tick = 0; tick < 60000; ++tick) {
            // Deterministic input script: plunge every 3 s for 1.6 s,
            // flipper mashing at 7-tick periods.
            tb::sim::TickInput in;
            const int phase = tick % 3000;
            if (phase >= 0 && phase < 1600) {
                in.buttons |= 1u << 4; // plunger held
            }
            if ((tick % 7) < 3) {
                in.buttons |= 1u; // left flipper
            }
            if ((tick % 11) < 4) {
                in.buttons |= 2u; // right flipper (also drives upper)
            }
            solver.step(sim, in);
            if ((tick + 1) % 5000 == 0) {
                hashes.push_back(tb::sim::state_hash(sim));
            }
        }
        return hashes;
    };

    const std::vector<uint64_t> a = run_greybox();
    const std::vector<uint64_t> b = run_greybox();
    ASSERT_EQ(a.size(), 12u);
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "greybox diverged at sample " << i;
    }
}

// Prefab.FlipperPairExpansionGolden: the §5.1 expansion of
// flipper_pair_standard at the test-lab position matches the committed
// golden JSON.
TEST(Prefab, FlipperPairExpansionGolden) {
    tb::table::TableDef partial; // boundary size only; no elements needed
    partial.width = 0.52f;
    partial.height = 1.04f;
    tb::table::PrefabInstance inst;
    inst.id = "flippers";
    inst.prefab = "flipper_pair_standard";
    inst.pos[0] = 0.240f;
    inst.pos[1] = 0.115f;

    const std::vector<tb::table::Element> kids = tb::table::expand_prefab(partial, inst);
    ASSERT_EQ(kids.size(), 2u);

    // The fixture is authoritative: parse it and compare field-by-field.
    std::ifstream in(tb::test::data_path("tests/fixtures/flipper_pair_golden.json"));
    ASSERT_TRUE(in.good());
    nlohmann::json golden;
    try {
        golden = nlohmann::json::parse(in, nullptr, true, true); // comments on
    } catch (const std::exception&) {
        FAIL() << "golden fixture is not valid JSON";
    }

    const auto& left = std::get<tb::table::FlipperDef>(kids[0].def);
    const auto& right = std::get<tb::table::FlipperDef>(kids[1].def);
    const auto& g_left = golden.at("left");
    const auto& g_right = golden.at("right");
    EXPECT_EQ(left.id, g_left.at("id").get<std::string>());
    EXPECT_NEAR(left.pos[0], g_left.at("pos")[0].get<float>(), 2e-6f);
    EXPECT_NEAR(left.pos[1], g_left.at("pos")[1].get<float>(), 1e-7f);
    EXPECT_FLOAT_EQ(left.rest_angle_deg, g_left.at("rest_angle_deg").get<float>());
    EXPECT_TRUE(left.left_side);
    EXPECT_EQ(left.input, g_left.at("input").get<std::string>());
    EXPECT_FLOAT_EQ(left.length, g_left.at("length").get<float>());
    EXPECT_EQ(right.id, g_right.at("id").get<std::string>());
    EXPECT_NEAR(right.pos[0], g_right.at("pos")[0].get<float>(), 2e-6f);
    EXPECT_NEAR(right.pos[1], g_right.at("pos")[1].get<float>(), 1e-7f);
    EXPECT_FLOAT_EQ(right.rest_angle_deg, g_right.at("rest_angle_deg").get<float>());
    EXPECT_FALSE(right.left_side);
    EXPECT_EQ(right.input, g_right.at("input").get<std::string>());
}

// Malformed-type inputs must surface as TableLoadError with a pointer —
// never a raw nlohmann exception and never a crash (review cycle 1).
TEST(TableLoader, MalformedTypesArePathQualified) {
    const std::string base = R"JSON({
      "format_version": 1,
      "meta": { "slug": "m", "name": "M", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 4 })JSON";

    struct Case {
        std::string body;
        const char* needle;
    };

    const Case cases[] = {
        {base + R"JSON(,
        { "id": "sg", "type": "slingshot", "face": [1, 2] }]
    })JSON",
         "/elements/3/face"},
        {base + R"JSON(,
        { "id": "w", "type": "wall",
          "path": [[0.1,0.5],{"arc":{"to":[0.2,0.6],"radius":"big","dir":"cw"}}] }]
    })JSON",
         "/path/1/arc"},
        {R"JSON({
      "format_version": 1,
      "meta": { "slug": "m", "name": "M", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "playfield": { "size": ["0.52", 1.04] },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 4 }]
    })JSON",
         "/playfield/size"},
        {R"JSON({
      "format_version": 1,
      "meta": { "slug": "m", "name": "M", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "materials": [1, 2],
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 4 }]
    })JSON",
         "/materials"},
        {base + R"JSON(,
        { "id": "p", "type": "post", "pos": [0.1, 0.5], "layer": 1.9 }]
    })JSON",
         "/layer"},
        {base + R"JSON(,
        { "id": "t2", "type": "trough", "capacity": 0 }]
    })JSON",
         "/capacity"},
        {base + R"JSON(]
      ,
      "prefabs": [{ "id": "x", "prefab": "nonesuch" }]
    })JSON",
         "/prefabs/0"},
    };

    for (const Case& c : cases) {
        std::filesystem::path dir;
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("tb_mt_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir, ec);
        { std::ofstream(dir / "table.json") << c.body; }
        bool threw = false;
        try {
            tb::table::load_table(dir);
        } catch (const tb::table::TableLoadError& e) {
            threw = true;
            // The pointer lives on the exception's json_pointer field (the
            // what() text may predate index enrichment).
            EXPECT_NE(e.json_pointer.find(c.needle), std::string::npos)
                << "expected pointer " << c.needle << ", got " << e.json_pointer << " (" << e.what()
                << ")";
        } catch (const std::exception& e) {
            ADD_FAILURE() << "case '" << c.needle << "' escaped as " << e.what();
        }
        std::filesystem::remove_all(dir, ec);
        ASSERT_TRUE(threw) << "case '" << c.needle << "' did not throw";
    }
}

// Degenerate arcs (|to − from| < 0.001) are V019 load errors (§3).
TEST(TableLoader, DegenerateArcRejected) {
    const std::string json = R"JSON({
      "format_version": 1,
      "meta": { "slug": "d", "name": "D", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "w", "type": "wall",
          "path": [[0.1, 0.5],
                   {"arc": {"to": [0.1005, 0.5], "radius": 0.05, "dir": "cw"}}] }
      ]
    })JSON";
    std::filesystem::path dir;
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / ("tb_deg_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "table.json") << json;
    bool threw = false;
    try {
        tb::table::load_table(dir);
    } catch (const tb::table::TableLoadError&) {
        threw = true;
    }
    std::filesystem::remove_all(dir, ec);
    EXPECT_TRUE(threw);
}

// sling_pair: the _left_ children sit at smaller x than _right_ (review
// cycle 1 — the sides were swapped).
TEST(Prefab, SlingPairSidesCorrect) {
    tb::table::TableDef partial;
    partial.width = 0.52f;
    partial.height = 1.04f;
    tb::table::PrefabInstance inst;
    inst.id = "sl";
    inst.prefab = "sling_pair";
    inst.pos[0] = 0.240f;
    inst.pos[1] = 0.210f;
    inst.sling_spread = 0.150f;

    const std::vector<tb::table::Element> kids = tb::table::expand_prefab(partial, inst);
    ASSERT_EQ(kids.size(), 4u);
    const auto& left_wall = std::get<tb::table::WallDef>(kids[0].def);
    const auto& right_wall = std::get<tb::table::WallDef>(kids[1].def);
    ASSERT_EQ(left_wall.id, "sl_left_wall");
    ASSERT_EQ(right_wall.id, "sl_right_wall");
    // Bottom corner B: left at pos.x − spread/2, right mirrored.
    EXPECT_NEAR(left_wall.path[0].point[0], 0.240f - 0.075f, 1e-6f);
    EXPECT_NEAR(right_wall.path[0].point[0], 0.240f + 0.075f, 1e-6f);
    EXPECT_FLOAT_EQ(left_wall.path[0].point[1], right_wall.path[0].point[1]);
}

// The lane/orbit merge decision is order-independent: a plunger_lane
// declared BEFORE the orbit still drops its top post (§5.2 measurement
// rule; review cycle 1).
TEST(Prefab, LaneOrbitMergeIsOrderIndependent) {
    const std::string json = R"JSON({
      "format_version": 1,
      "meta": { "slug": "o", "name": "O", "theme": "t", "author": "a",
                "description": "d", "rules_card": "r" },
      "elements": [
        { "id": "outer", "type": "wall", "closed": true,
          "path": [[0,0],[0,1],[0.5,1],[0.5,0]] },
        { "id": "drain", "type": "outhole",
          "region": { "a": [0.05, 0.01], "b": [0.4, 0.01] } },
        { "id": "tr", "type": "trough", "capacity": 4 }
      ],
      "prefabs": [
        { "id": "sh", "prefab": "plunger_lane" },
        { "id": "ob", "prefab": "orbit" }
      ]
    })JSON";
    std::filesystem::path dir;
    std::error_code ec;
    dir = std::filesystem::temp_directory_path(ec) / ("tb_ord_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "table.json") << json;
    bool has_top_post = true;
    try {
        const tb::table::TableDef def = tb::table::load_table(dir);
        has_top_post = false;
        for (const auto& e : def.elements) {
            if (e.id() == "sh_top_post") {
                has_top_post = true;
            }
        }
    } catch (const tb::table::TableLoadError& e) {
        FAIL() << "load failed: " << e.what();
    }
    std::filesystem::remove_all(dir, ec);
    EXPECT_FALSE(has_top_post) << "lane declared before orbit must still merge";
}
