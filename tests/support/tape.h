#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// JSON test-tape loader (16-testing-ci.md §2.4.2 format). Lives in test
// support because tb_sim cannot link nlohmann-json (canon §5.1); the tape
// expands into the 05 §13.1 edge mapping before injection.
namespace tb::test {

struct Tape {
    uint64_t seed = 0;
    std::string table_slug;
    // buttons word per tick, sized to cover the last entry's tick.
    std::vector<uint32_t> buttons_by_tick;
};

// Bit → action mapping per 05-engine-core.md §13.1: bits 0–4 and 6–9 map
// 1:1 onto action indices; bit 5 ("launch") ORs into bit 4 (plunger).
uint32_t tape_mask_to_buttons(uint32_t mask);

// Parses a .replay.json tape; returns false on malformed input.
bool load_tape(const std::filesystem::path& path, Tape& out);

} // namespace tb::test
