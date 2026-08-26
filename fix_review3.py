import io

p = "tests/support/tape.cpp"
s = open(p, encoding="utf-8").read()

old_map = """uint32_t tape_mask_to_buttons(uint32_t mask) {
    // Bits 0..4 -> actions 0..4; bit 5 aliases action 4 (OR); bits 6..8 ->
    // actions 5..7; bit 9 -> action 8. Bits 10-15 are reserved.
    uint32_t buttons = mask & 0x1Fu;         // actions 0..4
    buttons |= ((mask >> 5) & 1u) << 4;      // launch alias ORs plunger
    buttons |= (mask & (0x7u << 6));         // nudge_left/right/up stay
    buttons |= (mask & (1u << 9));           // start
    return buttons;
}"""
if old_map not in s:
    # clang-format may have rewrapped; find by unique substrings
    raise SystemExit("map block not found")

new_map = """uint32_t tape_mask_to_buttons(uint32_t mask) {
    // Bit->action mapping per 05-engine-core.md \u00a713.1: bits 0\u20134 map 1:1;
    // bit 5 ("launch") ORs into action 4; bits 6\u20138 shift DOWN one to
    // actions 5\u20137 (nudges); bit 9 shifts to action 8 (start). Bits
    // 10\u201315 are reserved and rejected by the loader.
    uint32_t buttons = mask & 0x1Fu;              // actions 0..4
    buttons |= ((mask >> 5) & 1u) << 4;           // launch alias ORs plunger
    buttons |= (mask & (0x7u << 6)) >> 1;          // bits 6..8 -> actions 5..7
    buttons |= (mask & (1u << 9)) >> 1;            // bit 9 -> action 8
    return buttons;
}"""
s = s.replace(old_map, new_map)

old_parse = """        if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number()
            || !entry[1].is_number()) {
            return false;
        }
        const uint64_t tick = entry[0].get<uint64_t>();
        const uint32_t mask = entry[1].get<uint32_t>();
        if (tick < last_tick) {
            return false; // strictly increasing ticks
        }"""
new_parse = """        if (!entry.is_array() || entry.size() != 2
            || !entry[0].is_number_unsigned()
            || !entry[1].is_number_unsigned()) {
            return false;
        }
        const uint64_t tick = entry[0].get<uint64_t>();
        constexpr uint64_t kMaxTapeTick = 100'000'000; // sanity cap
        if (tick < last_tick || tick >= kMaxTapeTick) {
            return false; // strictly increasing, bounded length
        }
        const uint32_t mask = entry[1].get<uint32_t>();"""
assert old_parse in s
s = s.replace(old_parse, new_parse)
open(p, "w", encoding="utf-8").write(s)
print("tape ok")
