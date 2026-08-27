#include "game/score_format.h"

#include <string>

namespace tb::game {

std::string format_score(uint64_t score) {
    if (score == 0) {
        return "00"; // §6: zero renders as the classic 00
    }
    std::string digits;
    {
        std::string raw = std::to_string(score);
        // Group from the right, inserting commas every 3 digits.
        size_t lead = raw.size() % 3;
        if (lead == 0) {
            lead = 3;
        }
        digits.append(raw, 0, lead);
        for (size_t i = lead; i < raw.size(); i += 3) {
            digits.push_back(',');
            digits.append(raw, i, 3);
        }
    }
    return digits;
}

} // namespace tb::game
