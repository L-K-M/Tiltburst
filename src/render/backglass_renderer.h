#pragma once

#include "render/renderer.h"
#include "sim/script_host.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tb::render {

// Backglass content v1 (04-milestones.md M12, 07 §10/§13 §9.2): the
// layout pass turns the live game state + BackglassModel into a flat
// quad list in backglass-canvas pixel space (640×512, 13-art-direction
// §2). The GPU path (SdlGpuRenderer::render_backglass) draws exactly
// these quads; the same list feeds the single-display B-key overlay.
struct BackglassContent {
    // Game state inputs (read on the main thread from the same
    // snapshot the playfield frame used — no extra sim access).
    int player_count = 1;
    int current_player = 1; // 1-based
    int ball_number = 1;
    uint64_t scores[4] = {}; // player_count entries valid
    bool in_attract = true;  // attract shows the high-score list

    // Attract high scores (11 §7): up to 10 rows.
    struct HighScoreRow {
        char initials[4] = {0, 0, 0, 0};
        uint64_t score = 0;
    };

    HighScoreRow high_scores[10];
    uint32_t high_score_count = 0;
};

// Produces the frame's draw list: score cards (active player marked),
// status band (ball number), message ticker, attract high-score list.
// Text glyphs come from the overlay's stb_easy_font emitter; callers
// pass the Overlay instance they already own.
class BackglassLayout {
public:
    // Appends background + text quads to out. Deterministic in its
    // inputs (no time dependence — blink/marquee are M13 polish).
    void build(const BackglassContent& content,
               const sim::BackglassModel& model,
               const class Overlay& font,
               std::vector<QuadInstance>* out) const;
};

} // namespace tb::render
