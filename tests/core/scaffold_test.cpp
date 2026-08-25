#include "core/version.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>

// 04-milestones.md M0: the first test in the repo. The id is quoted
// verbatim from 16-testing-ci.md §2/§3.2 (taxonomy `unit_`).
TEST(unit_scaffold, sanity) {
    const std::string version = tb::version_string();
    EXPECT_FALSE(version.empty());
    EXPECT_TRUE(std::regex_match(version, std::regex(R"(^\d+\.\d+\.\d+$)")));
}
