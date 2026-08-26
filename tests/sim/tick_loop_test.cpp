#include "core/time.h"
#include "sim/sim_thread.h"

#include <gtest/gtest.h>

#include <atomic>

namespace {

// Fake clock. Two properties the harness needs (16-testing-ci.md: a stall
// here is a harness bug, never a sim bug):
//   * reads always flow forward (+1 µs per read) so the absolute-schedule
//     loop can drain any accumulated debt;
//   * wait() jumps straight to the target so a full simulated second costs
//     microseconds of wall time.
std::atomic<uint64_t> g_fake_ns{0};

uint64_t fake_now() {
    return g_fake_ns.fetch_add(1'000, std::memory_order_relaxed) + 1'000;
}

void fake_wait(uint64_t target_ns) {
    uint64_t cur = g_fake_ns.load(std::memory_order_relaxed);
    while (cur < target_ns &&
           !g_fake_ns.compare_exchange_weak(cur, target_ns, std::memory_order_relaxed)) {}
}

void reset_clock() {
    g_fake_ns.store(0, std::memory_order_relaxed);
}

} // namespace

// TickLoop.Produces1000TicksPerSimSecond: 5000 simulated ms produce 5000
// ticks (canon §5.3: dt = 0.001 exactly).
TEST(unit_tick_loop, produces_1000_ticks_per_sim_second) {
    reset_clock();

    tb::SimThread sim;
    sim.set_clock(&fake_now);
    sim.set_wait(&fake_wait);

    std::atomic<uint64_t> ticks{0};
    sim.start([&sim, &ticks](uint64_t) {
        if (ticks.fetch_add(1) == 4999) {
            // Self-stop from inside the tick: no cross-thread stop latency
            // to race against on a loaded runner (macOS CI found this).
            sim.request_stop();
        }
    });
    sim.join();

    // Exact only because the loop honors stop between every tick,
    // including inside a catch-up burst.
    const uint64_t n = ticks.load();
    EXPECT_EQ(n, 5000u);
    // 5000 ticks = 5 simulated seconds; the clock may run a hair past.
    EXPECT_NEAR(double(g_fake_ns.load()) / 1e9, 5.0, 0.25);
}

// §6.1 overrun clamp: a clock jump produces exactly one clamped burst —
// at most 50 catch-up ticks, remainder dropped, warn counted.
TEST(unit_tick_loop, overrun_clamps_at_50) {
    reset_clock();

    tb::SimThread sim;
    sim.set_clock(&fake_now);
    sim.set_wait(&fake_wait);

    std::atomic<uint64_t> ticks{0};
    std::atomic<bool> stop_requested{false};
    sim.start([&sim, &ticks, &stop_requested](uint64_t) {
        const uint64_t n = ticks.fetch_add(1);
        if (n == 9) {
            // Simulate a stall: jump the clock forward by 200 ms.
            fake_wait(fake_now() + 200'000'000ull);
        }
        if (n >= 70 && !stop_requested.exchange(true)) {
            // Self-stop well past the clamp burst (which ends around tick
            // 60) so overrun/drop accounting is final before join.
            sim.request_stop();
        }
    });

    // Bounded observation: if the overflow policy regresses and the loop
    // stalls before tick 71, fail fast instead of hanging CI.
    int64_t guard = 0;
    while (ticks.load(std::memory_order_relaxed) < 71 && !stop_requested.load()) {
        tb::cpu_pause();
        ASSERT_LT(++guard, 200'000'000ll) << "sim stalled; overflow policy regressed";
    }
    sim.join();

    EXPECT_EQ(sim.overruns(), 1u);
    EXPECT_EQ(sim.dropped_ticks(), 150u);
    EXPECT_GE(ticks.load(), 60u);
}
