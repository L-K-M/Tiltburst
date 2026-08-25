#include "core/rng.h"

#include <gtest/gtest.h>

// 05-engine-core.md §10.1: reference sequence pins the implementation.
TEST(unit_rng, known_sequence) {
    tb::Pcg32 rng;
    rng.seed(42u, 54u);
    EXPECT_EQ(rng.next_u32(), 0xa15c02b7u);
    EXPECT_EQ(rng.next_u32(), 0x7b47f409u);
    EXPECT_EQ(rng.next_u32(), 0xba1d3330u);
}

// §10.1 / Done-when: next_below passes a chi-squared uniformity test for
// bound = 6 over 1e6 draws (p > 0.001 ⇒ chi2 below the df=5 critical
// value 20.52).
TEST(unit_rng, next_below_uniform) {
    tb::Pcg32 rng;
    rng.seed(0x54425354u, 1u);

    constexpr uint32_t kBound = 6;
    constexpr int kDraws = 1000000;
    double expected = double(kDraws) / double(kBound);
    double chi2 = 0.0;

    uint64_t counts[kBound] = {};
    for (int i = 0; i < kDraws; ++i) {
        counts[rng.next_below(kBound)]++;
    }
    for (uint32_t b = 0; b < kBound; ++b) {
        const double diff = double(counts[b]) - expected;
        chi2 += diff * diff / expected;
    }
    EXPECT_LT(chi2, 20.52) << "chi2=" << chi2;

    // Every draw stays strictly inside the bound.
    for (int i = 0; i < 10000; ++i) {
        ASSERT_LT(rng.next_below(kBound), kBound);
    }
}
