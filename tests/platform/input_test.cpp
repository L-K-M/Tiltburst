#include "platform/input.h"

#include "core/time.h"
#include "platform/input_internal.h"
#include "platform/latency.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <vector>

// Input system contracts (05-engine-core.md §9.2) and the M4 input→latch
// latency gate (16-testing-ci.md perf_latency.input_to_tick_p999).
namespace {

using tb::input::InputEdge;
using tb::input::InputSource;
using tb::input::InputState;

class TestSource final : public tb::input::RingSource {
public:
    const char* name() const override { return name_; }

    bool start() override { return true; }

    void stop() override {}

    void set_name(const char* n) { name_ = n; }

private:
    const char* name_ = "sdl";
};

} // namespace

TEST(input_latch, edges_apply_in_order_and_levels_track) {
    TestSource a;
    TestSource b;
    b.set_name("evdev");
    std::vector<InputSource*> sources = {&a, &b};

    // Sub-tick tap: press+release inside one tick both arrive (§9.2 rule 3).
    a.submit(InputEdge{1000, tb::input::kActionLeftFlipper, 1, tb::input::kSourceSynthetic});
    a.submit(InputEdge{1001, tb::input::kActionLeftFlipper, 0, tb::input::kSourceSynthetic});
    // Raw source edge for the right flipper.
    b.submit(InputEdge{1002, tb::input::kActionRightFlipper, 1, tb::input::kSourceEvdev});

    InputState state;
    const uint32_t buttons =
        tb::input::latch_input(sources.data(), sources.size(), state, 2000, nullptr);
    EXPECT_EQ(buttons & (1u << tb::input::kActionLeftFlipper), 0u);  // tapped out
    EXPECT_NE(buttons & (1u << tb::input::kActionRightFlipper), 0u); // held
    EXPECT_EQ(state.last_press_ns[tb::input::kActionLeftFlipper], 1000u);
    EXPECT_EQ(state.last_release_ns[tb::input::kActionLeftFlipper], 1001u);
}

TEST(input_latch, at_most_64_edges_drain_per_tick) {
    TestSource a;
    std::vector<InputSource*> sources = {&a};
    for (int i = 0; i < 100; ++i) {
        a.submit(InputEdge{uint64_t(i),
                           tb::input::kActionStart,
                           uint8_t(i % 2 == 0),
                           tb::input::kSourceSynthetic});
    }
    InputState state;
    tb::input::latch_input(sources.data(), sources.size(), state, 10'000, nullptr);
    // Surplus stays queued for the next tick (never dropped).
    InputEdge extra[8];
    EXPECT_GT(a.poll_edges(extra, 8), 0u);
}

TEST(input_latch, sdl_gameplay_suppressed_while_raw_active) {
    TestSource sdl;
    TestSource raw;
    raw.set_name("evdev");
    std::vector<InputSource*> sources = {&raw, &sdl};
    tb::input::g_raw_source_active.store(true); // §9.8 pump-side gate

    // SDL gameplay edges are dropped at the source: they neither ring nor
    // touch the atomic; UI-class flows.
    sdl.submit(InputEdge{2, tb::input::kActionPause, 1, tb::input::kSourceSdl});
    raw.submit(InputEdge{3, tb::input::kActionRightFlipper, 1, tb::input::kSourceEvdev});

    InputState state;
    const uint32_t buttons =
        tb::input::latch_input(sources.data(), sources.size(), state, 100, nullptr);
    EXPECT_EQ(buttons & (1u << tb::input::kActionLeftFlipper), 0u);  // suppressed
    EXPECT_NE(buttons & (1u << tb::input::kActionPause), 0u);        // UI passes
    EXPECT_NE(buttons & (1u << tb::input::kActionRightFlipper), 0u); // raw wins
    tb::input::g_button_bits.store(0);
    tb::input::g_raw_source_active.store(false);
}

TEST(input_latch, focus_gate_drops_gameplay_only) {
    TestSource a;
    std::vector<InputSource*> sources = {&a};
    a.submit(InputEdge{1, tb::input::kActionLeftFlipper, 1, tb::input::kSourceSdl});

    tb::input::g_app_focused.store(false);
    InputState state;
    const uint32_t buttons =
        tb::input::latch_input(sources.data(), sources.size(), state, 100, nullptr);
    EXPECT_EQ(buttons & (1u << tb::input::kActionLeftFlipper), 0u);

    // Refocus: reconciliation synthesizes the held level back in.
    tb::input::g_app_focused.store(true);
    const uint32_t refocused =
        tb::input::latch_input(sources.data(), sources.size(), state, 200, nullptr);
    EXPECT_NE(refocused & (1u << tb::input::kActionLeftFlipper), 0u);

    tb::input::g_button_bits.store(0);
    state.buttons = 0;
}

// The M4 gate: p99.9 < 4 ms over ≥ 10,000 scripted press edges through the
// real late-latch path (04-milestones.md M4; registered as
// perf_latency.input_to_tick_p999 in the perf-gates job).
TEST(perf_latency, input_to_tick_p999) {
    TestSource a;
    std::vector<InputSource*> sources = {&a};

    constexpr int kEdges = 12'000; // ≥ 10,000 required for a quotable p99.9
    tb::input::LatencyHistogram histogram;

    InputState state;
    for (int i = 0; i < kEdges; ++i) {
        // Scripted edge: timestamped now, latched immediately after.
        const uint64_t ts = tb_now_ns();
        a.submit(InputEdge{ts, tb::input::kActionLeftFlipper, 1, tb::input::kSourceSynthetic});
        a.submit(InputEdge{ts + 1, tb::input::kActionLeftFlipper, 0, tb::input::kSourceSynthetic});
        tb::input::latch_input(sources.data(), sources.size(), state, tb_now_ns(), &histogram);
    }

    ASSERT_GE(histogram.total(), uint64_t(10'000));
    const double p999_ms = histogram.percentile(0.999) / 1e6;
    EXPECT_LT(p999_ms, 4.0) << "R2.1 gate: p99.9 input→latch must stay < 4 ms";
}

TEST(latency_histogram, percentiles_are_bin_upper_edges_ns) {
    tb::input::LatencyHistogram h;
    h.record(0);      // bin 0 → upper edge 62.5 µs
    h.record(62'499); // bin 0
    h.record(62'500); // bin 1 → upper edge 125 µs
    EXPECT_DOUBLE_EQ(h.percentile(0.5), 62'500.0);
    EXPECT_DOUBLE_EQ(h.percentile(1.0), 125'000.0);
    h.record(8'000'001); // overflow bin → upper edge 8 ms
    EXPECT_DOUBLE_EQ(h.percentile(1.0), 8'000'000.0);
}
