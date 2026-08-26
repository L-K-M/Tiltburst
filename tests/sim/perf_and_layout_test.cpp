#include "sim/solver.h"
#include "support/alloc_hook.h"
#include "support/data_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace {

// Median-of-3 helper shared by the perf harness.
double median_of(std::array<double, 3> v) {
    std::sort(v.begin(), v.end());
    return v[1];
}

} // namespace

// HotPath.NoAllocationsInStep: after a warm-up, Solver::step must not
// allocate or free (03-process.md §1.6; 08 §9 item 2).
TEST(unit_hot_path, no_allocations_in_step) {
    tb::sim::SimState s;
    tb::sim::make_synthetic_scene(s, 99);
    s.slope_deg = 6.5f;

    tb::sim::Solver solver;
    const tb::sim::TickInput input;
    for (int i = 0; i < 1000; ++i) {
        solver.step(s, input); // warm-up: touch all code paths
    }

    tb::test::ScopedAllocCount count;
    for (int i = 0; i < 1000; ++i) {
        solver.step(s, input);
    }
    EXPECT_EQ(count.news_total(), 0u);
    EXPECT_EQ(count.deletes_total(), 0u);
}

// HotPath.NoAllocationsWithFlippers: the M4 flipper path (stroke state
// machine, moving-capsule CCD, persistent-contact probes) must hold the
// same allocation-free contract under mashing input.
TEST(unit_hot_path, no_allocations_with_flippers) {
    tb::sim::SimState s;
    tb::sim::make_synthetic_scene(s, 99);
    s.slope_deg = 6.5f;

    // Two flippers into the post lattice.
    tb::sim::Flipper fl{};
    fl.params.pivot = {0.170f, 0.120f};
    fl.params.rest_angle_deg = -31.0f;
    fl.params.side_sign = +1;
    fl.params.action = 0;
    fl.theta = fl.params.rest_rad();
    fl.theta_start = fl.theta;
    s.flippers.push_back(fl);

    tb::sim::Solver solver;
    uint32_t mash = 1; // deterministic button pattern
    auto pressed = [](uint32_t pattern, int tick) {
        return ((pattern >> uint32_t(tick % 7)) & 1u) != 0u;
    };
    for (int i = 0; i < 1000; ++i) {
        tb::sim::TickInput in;
        in.buttons = pressed(mash, i) ? 1u : 0u;
        solver.step(s, in); // warm-up
    }

    tb::test::ScopedAllocCount count;
    for (int i = 0; i < 1000; ++i) {
        tb::sim::TickInput in;
        in.buttons = pressed(mash, i) ? 1u : 0u;
        solver.step(s, in);
    }
    EXPECT_EQ(count.news_total(), 0u);
    EXPECT_EQ(count.deletes_total(), 0u);
}

// Layout.SimIncludesNothingForbidden: no src/sim include may reference
// render/platform/audio/SDL (canon §5.1: tb_sim links only tb_core).
TEST(unit_layout, sim_includes_nothing_forbidden) {
    const std::filesystem::path dir = tb::test::data_path("src/sim");
    ASSERT_TRUE(std::filesystem::exists(dir));

    const std::regex forbidden("(render/|platform/|audio/|<SDL)", std::regex_constants::icase);

    int scanned = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("#include", 0) != 0) {
                continue;
            }
            EXPECT_FALSE(std::regex_search(line, forbidden))
                << entry.path().string() << ": " << line;
        }
        ++scanned;
    }
    EXPECT_GT(scanned, 5) << "layout scan found nothing to check";
}

// perf_tick.gate_synthetic — the first rung of the perf ladder
// (16-testing-ci.md §2.9): the 88-collider synthetic scene, 4 balls,
// median-of-3 runs of 60,000 timed ticks after 5,000 warmup.
// Limits: mean < 100 µs, p99 < 200 µs. Release-only by definition.
TEST(perf_tick, gate_synthetic) {
#ifndef NDEBUG
    GTEST_SKIP() << "perf gates are Release-only";
#else
    using Clock = std::chrono::steady_clock;

    double mean_us[3] = {};
    double p99_us[3] = {};

    for (int run = 0; run < 3; ++run) {
        tb::sim::SimState s;
        tb::sim::make_synthetic_scene(s, uint64_t(99) + run * 1000);

        tb::sim::Solver solver;
        const tb::sim::TickInput input;

        // Warmup: 5,000 untimed ticks.
        for (int i = 0; i < 5000; ++i) {
            solver.step(s, input);
        }

        constexpr int kTimed = 60000;
        std::vector<double> samples;
        samples.resize(size_t(kTimed));
        for (int i = 0; i < kTimed; ++i) {
            const auto t0 = Clock::now();
            solver.step(s, input);
            samples[size_t(i)] =
                double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0)
                           .count()) /
                1000.0;
        }

        std::sort(samples.begin(), samples.end());
        double sum = 0.0;
        for (double v : samples) {
            sum += v;
        }
        mean_us[run] = sum / double(kTimed);
        p99_us[run] = samples[size_t(std::ceil(0.99 * double(kTimed))) - 1];
    }

    const double mean = median_of({mean_us[0], mean_us[1], mean_us[2]});
    const double p99 = median_of({p99_us[0], p99_us[1], p99_us[2]});

    // Machine-readable report (16 §2.9).
    {
        std::ofstream out("perf_report.json", std::ios::app);
        out << "{\"gate\":\"perf_tick.gate_synthetic\",\"mean_us\":" << mean
            << ",\"p99_us\":" << p99 << "}\n";
    }

    EXPECT_LT(mean, 100.0) << "median-of-3 mean tick time exceeded 100 µs";
    EXPECT_LT(p99, 200.0) << "median-of-3 p99 tick time exceeded 200 µs";
#endif
}
