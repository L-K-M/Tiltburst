#include "support/tape.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tb::test {

uint32_t tape_mask_to_buttons(uint32_t mask) {
    // Bits 0..4 → actions 0..4; bit 5 aliases action 4 (OR); bits 6..8 →
    // actions 5..7; bit 9 → action 8. Bits 10–15 are reserved.
    uint32_t buttons = mask & 0x1Fu;    // actions 0..4
    buttons |= ((mask >> 5) & 1u) << 4; // launch alias ORs plunger
    buttons |= (mask & (0x7u << 6));    // nudge_left/right/up stay
    buttons |= (mask & (1u << 9));      // start
    return buttons;
}

bool load_tape(const std::filesystem::path& path, Tape& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(in, nullptr, true, true);
    } catch (const std::exception&) {
        return false;
    }

    if (!doc.is_object() || !doc.contains("inputs") || !doc["inputs"].is_array()) {
        return false;
    }

    out.seed = doc.value("seed", uint64_t(0));
    out.table_slug = doc.value("table_slug", std::string());

    uint64_t last_tick = 0;
    for (const auto& entry : doc["inputs"]) {
        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_unsigned() ||
            !entry[1].is_number_unsigned()) {
            return false;
        }
        const uint64_t tick = entry[0].get<uint64_t>();
        constexpr uint64_t kMaxTapeTick = 100000000; // sanity cap
        if (tick < last_tick || tick >= kMaxTapeTick) {
            return false; // strictly increasing, bounded length
        }
        const uint32_t mask = entry[1].get<uint32_t>();
        last_tick = tick;
        out.buttons_by_tick.resize(size_t(tick) + 1,
                                   out.buttons_by_tick.empty() ? 0u : out.buttons_by_tick.back());
        out.buttons_by_tick[size_t(tick)] = tape_mask_to_buttons(mask);
    }
    return true;
}

} // namespace tb::test
