#include "core/hash.h"

#include <gtest/gtest.h>

#include <string>

// 16-testing-ci.md §2.1: FNV-1a 64 known answer.
TEST(unit_hash, fnv1a64_known_answer) {
    EXPECT_EQ(tb::fnv1a64_str("tb"), 0x08c82e07b56aadf3ull);

    // Byte-wise and string forms agree.
    const std::string s = "tb";
    EXPECT_EQ(tb::fnv1a64(s.data(), s.size()), tb::fnv1a64_str("tb"));
}
