#include "platform/input.h"
#include "sim/collider.h"
#include "sim/flipper.h"
#include "sim/solver.h"
#include "support/data_path.h"
#include "support/tape.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

// M4 determinism coverage (04-milestones.md M4, 16-testing-ci.md §2.4.3):
// det_replay.flipper_tape_hash_stable drives the committed flipper-heavy
// tape through the replay machinery (parse → §13.1 expansion →
// exact-tick input schedule) on the synthetic flipper rig and compares
// state hashes against the committed per-OS golden (ADR-013).
namespace {

std::vector<std::pair<uint64_t, uint64_t>> load_golden(const std::filesystem::path& path) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        out.emplace_back(std::stoull(line.substr(0, space)),
                         std::stoull(line.substr(space + 1), nullptr, 16));
    }
    return out;
}

struct FlipperRig {
    tb::sim::SimState state;

    explicit FlipperRig(uint64_t seed) {
        state.slope_deg = 6.5f;
        state.width = 0.52f;
        state.height = 1.04f;
        uint16_t sub = 0;
        auto wall = [&](tb::sim::Vec2 a, tb::sim::Vec2 b) {
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Segment;
            c.a = a;
            c.b = b;
            c.element_id = 100;
            c.sub_index = sub++;
            c.material = tb::sim::MaterialId::Wood;
            state.colliders.push_back(c);
        };
        wall({0, 0}, {0.52f, 0});
        wall({0.52f, 0}, {0.52f, 1.04f});
        wall({0.52f, 1.04f}, {0, 1.04f});
        wall({0, 1.04f}, {0, 0});

        tb::sim::Flipper fl{};
        fl.params.pivot = {0.170f, 0.120f};
        fl.params.rest_angle_deg = -31.0f;
        fl.params.swing_deg = 52.0f;
        fl.params.side_sign = +1;
        fl.params.action = 0;
        fl.theta = fl.params.rest_rad();
        fl.theta_start = fl.theta;
        state.flippers.push_back(fl);

        tb::sim::Flipper fr = fl;
        fr.params.pivot = {0.350f, 0.120f};
        fr.params.rest_angle_deg = 211.0f;
        fr.params.side_sign = -1;
        fr.params.action = 1;
        fr.theta = fr.params.rest_rad();
        fr.theta_start = fr.theta;
        state.flippers.push_back(fr);

        state.grid.build(state.colliders, state.width, state.height);

        tb::sim::Ball& b = state.balls[0];
        b.index = 0;
        b.live = true;
        b.mode = tb::sim::BallMode::Free;
        b.layer = 0;
        b.pos = {0.26f, 0.60f};
        b.vel = {0.35f, -0.6f};
        b.last_safe_pos = b.pos;
        state.rng_sim.seed(seed, 1);
    }
};

// Replays the tape for 3000 ticks and collects (tick, state_hash) at
// every 100-tick sample. The compare and record paths share it so the
// loop can never drift between them.
std::vector<std::pair<uint64_t, uint64_t>> run_flipper_tape(const tb::test::Tape& tape) {
    FlipperRig rig(tape.seed);
    tb::sim::Solver solver;
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (uint64_t tick = 1; tick <= 3000; ++tick) {
        tb::sim::TickInput in;
        in.buttons = tick - 1 < tape.buttons_by_tick.size() ? tape.buttons_by_tick[size_t(tick - 1)]
                                                            : tape.buttons_by_tick.back();
        solver.step(rig.state, in);
        if (tick % 100 == 0) {
            out.emplace_back(tick, tb::sim::state_hash(rig.state));
        }
    }
    return out;
}

} // namespace

TEST(det_replay, flipper_tape_hash_stable) {
    // TB_RECORD_GOLDEN=<path> regenerates instead of comparing (§2.4.4).
    if (const char* record = std::getenv("TB_RECORD_GOLDEN")) {
        tb::test::Tape tape;
        ASSERT_TRUE(tb::test::load_tape(
            tb::test::data_path("tests/fixtures/tapes/flipper_tap.replay.json"), tape));
        ASSERT_NE(record, "");
        const auto hashes = run_flipper_tape(tape);
        std::ofstream out(record);
        ASSERT_TRUE(out.is_open()) << "cannot write golden to " << record;
        out << "# tiltburst determinism golden v1\n";
        out << "# regenerated M10: state_hash now folds tilt-bob/abuse/nudge"
            << " envelope state, arm latches and coil gates (hash-scope"
               " change, JOURNAL M10; 16 §2.4.4)\n";
        out << "# table: rig(flipper) tape: flipper_tap.replay.json seed: " << tape.seed << "\n";
        for (const auto& [tick, hash] : hashes) {
            out << tick << " " << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec
                << "\n";
        }
        out.flush();
        ASSERT_TRUE(out.good()) << "golden write failed to " << record;
        SUCCEED() << "golden recorded";
        return;
    }
#if !defined(__linux__)
    // ADR-013: goldens are compared same-OS only; other platforms skip
    // until their goldens land via CI artifacts.
    GTEST_SKIP() << "no committed golden for this OS yet";
#else
    tb::test::Tape tape;
    ASSERT_TRUE(tb::test::load_tape(
        tb::test::data_path("tests/fixtures/tapes/flipper_tap.replay.json"), tape));
    EXPECT_EQ(tape.seed, 987654321ull);
    ASSERT_FALSE(tape.buttons_by_tick.empty());

    const auto golden =
        load_golden(tb::test::data_path("tests/golden/determinism/linux/flipper_tap.hashes"));
    ASSERT_GE(golden.size(), 6u);

    const auto hashes = run_flipper_tape(tape);
    ASSERT_GE(golden.size(), 6u);
    ASSERT_EQ(hashes.size(), 30u);
    for (size_t gi = 0; gi < golden.size(); ++gi) {
        ASSERT_LT(gi, hashes.size());
        ASSERT_EQ(hashes[size_t(golden[gi].first / 100 - 1)].first, golden[gi].first)
            << "sample cadence mismatch";
        ASSERT_EQ(hashes[size_t(golden[gi].first / 100 - 1)].second, golden[gi].second)
            << "hash divergence at tick " << golden[gi].first << " (replay machinery or sim drift)";
    }
#endif
}
