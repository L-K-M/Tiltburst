#pragma once

#include <cstdint>
#include <string>

namespace tb::game {

// Ledger cap (11-game-framework.md §6): 9,999,999,999 (10 digits).
inline constexpr uint64_t kScoreCap = 9'999'999'999ull;

// §6 display formatting: thousands separators with commas, no leading
// zeros, except a zero score renders as "00" (classic look). Used
// everywhere a score is printed.
std::string format_score(uint64_t score);

} // namespace tb::game
