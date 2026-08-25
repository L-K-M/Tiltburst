#include "core/time.h"

#include <gtest/gtest.h>

// The timebase is monotonic: successive reads never go backwards, and a
// short busy wait advances it (05-engine-core.md §3).
TEST(unit_time, monotonic_non_decreasing) {
    uint64_t prev = tb_now_ns();
    for (int i = 0; i < 10000; ++i) {
        const uint64_t now = tb_now_ns();
        ASSERT_GE(now, prev);
        prev = now;
    }

    const uint64_t start = tb_now_ns();
    while (tb_now_ns() - start < 1'000'000ull) {
        tb::cpu_pause();
    }
    EXPECT_GE(tb_now_ns() - start, 1'000'000ull);
}

// sleep_until_ns returns at or after the target (05 §6.2 sleep-then-spin).
TEST(unit_time, sleep_until_reaches_target) {
    const uint64_t target = tb_now_ns() + 2'000'000ull; // 2 ms
    tb::sleep_until_ns(target);
    EXPECT_GE(tb_now_ns(), target);
}
