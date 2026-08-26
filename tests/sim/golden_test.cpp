#include "sim/replay.h"
#include "sim/solver.h"
#include "support/data_path.h"
#include "support/tape.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// Runs the m2_bounce tape against the synthetic scene, hashing every
// 1,000 ticks over 100,000 ticks.
std::vector<uint64_t> run_m2_bounce_hashes(uint64_t seed) {
    tb::test::Tape tape;
    if (!tb::test::load_tape(tb::test::data_path(std::filesystem::path("tests/fixtures/tapes") /
                                                 "m2_bounce.replay.json"),
                             tape)) {
        return {};
    }

    tb::sim::SimState s;
    tb::sim::make_synthetic_scene(s, seed);
    s.seeded = true;

    tb::sim::Solver solver;
    std::vector<uint64_t> hashes;
    const size_t total = 100000;
    for (size_t t = 0; t < total; ++t) {
        tb::sim::TickInput in;
        in.buttons = tape.buttons_by_tick[std::min(t, tape.buttons_by_tick.size() - 1)];
        solver.step(s, in);
        if ((t + 1) % 1000 == 0) {
            hashes.push_back(tb::sim::state_hash(s));
        }
    }
    return hashes;
}

} // namespace

// det_golden.same_os_m2_bounce: compare the hash-per-1000 sequence against
// tests/golden/determinism/<os>/m2_bounce.hashes (ADR-013: same OS only).
// SKIPs when the golden for this OS has not been recorded yet (16 §2.4.3);
// TB_RECORD_GOLDEN=<path> regenerates it instead of comparing (§2.4.4).
TEST(det_golden, m2_bounce) {
    if (const char* record = std::getenv("TB_RECORD_GOLDEN")) {
        const auto hashes = run_m2_bounce_hashes(424242);
        ASSERT_EQ(hashes.size(), 100u);
        std::ofstream out(record);
        out << "# tiltburst determinism golden v1\n";
        out << "# table: test-lab(m2 synthetic) tape: m2_bounce.replay.json"
               " seed: 424242\n";
        char prev_fill = out.fill('0');
        auto prev_flags = out.flags();
        for (size_t i = 0; i < hashes.size(); ++i) {
            out << (i + 1) * 1000 << " " << std::hex << std::setw(16) << hashes[i] << std::dec
                << "\n";
        }
        out.fill(prev_fill);
        out.flags(prev_flags);
        GTEST_SKIP() << "golden recorded to " << record;
    }

    // Same-OS golden name from the platform this binary runs on.
#if defined(_WIN32)
    const char* os_dir = "windows";
#elif defined(__APPLE__)
    const char* os_dir = "macos";
#else
    const char* os_dir = "linux";
#endif

    const std::filesystem::path golden = tb::test::data_path(
        std::filesystem::path("tests/golden/determinism") / os_dir / "m2_bounce.hashes");
    if (!std::filesystem::exists(golden)) {
        GTEST_SKIP() << "no golden for " << os_dir << " yet; set TB_RECORD_GOLDEN to record one";
    }

    std::ifstream in(golden);
    ASSERT_TRUE(in.good()) << "cannot open golden file: " << golden;
    std::vector<uint64_t> want;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("#", 0) == 0 || line.empty()) {
            continue;
        }
        const unsigned long long tick = std::strtoul(line.c_str(), nullptr, 10);
        const unsigned long long hash =
            std::strtoull(line.c_str() + line.find(' ') + 1, nullptr, 16);
        EXPECT_EQ(tick % 1000, 0ull);
        want.push_back(hash);
    }
    ASSERT_EQ(want.size(), 100u);

    const auto got = run_m2_bounce_hashes(424242);
    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(got[i], want[i]) << "diverged at sample " << i;
    }
}
