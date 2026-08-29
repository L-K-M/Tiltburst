#include "render/backglass_renderer.h"

#include "game/score_format.h"
#include "render/overlay.h"

#include <algorithm>
#include <cstdio>

namespace tb::render {

namespace {

// Palette v1 (13-art-direction.md owns the final look; these are the
// readable-greybox tones until M13).
constexpr float kBgR = 0.04f, kBgG = 0.05f, kBgB = 0.08f;
constexpr float kTextR = 0.92f, kTextG = 0.94f, kTextB = 0.98f;
constexpr float kDimR = 0.45f, kDimG = 0.48f, kDimB = 0.55f;
constexpr float kActiveR = 1.0f, kActiveG = 0.82f, kActiveB = 0.25f;
constexpr float kMsgStyle[4][3] = {
    {0.92f, 0.94f, 0.98f}, // info
    {0.35f, 0.85f, 1.0f},  // mode
    {1.0f, 0.82f, 0.25f},  // jackpot
    {1.0f, 0.35f, 0.25f},  // warning
};

void quad(float cx,
          float cy,
          float hx,
          float hy,
          float r,
          float g,
          float b,
          float a,
          std::vector<QuadInstance>* out) {
    QuadInstance q;
    q.cx = cx;
    q.cy = cy;
    q.hx = hx;
    q.hy = hy;
    q.r = r;
    q.g = g;
    q.b = b;
    q.a = a;
    out->push_back(q);
}

void text(const Overlay& font,
          float x,
          float y,
          const std::string& s,
          float r,
          float g,
          float b,
          std::vector<QuadInstance>* out) {
    font.emit_quads(x, y, s, r, g, b, 1.0f, out, BackglassFrame::kCanvasH);
}

} // namespace

void BackglassLayout::build(const BackglassContent& content,
                            const sim::BackglassModel& model,
                            const Overlay& font,
                            std::vector<QuadInstance>* out) const {
    const float W = float(BackglassFrame::kCanvasW);
    const float H = float(BackglassFrame::kCanvasH);

    // Background.
    quad(W * 0.5f, H * 0.5f, W * 0.5f, H * 0.5f, kBgR, kBgG, kBgB, 1.0f, out);

    if (content.in_attract) {
        // §9.2 Attract: high scores, rank 1 highlighted, paged layout
        // is M13 polish — v1 lists up to 10 rows.
        text(font, 24.0f, 24.0f, "HIGH SCORES", kTextR, kTextG, kTextB, out);
        float y = 60.0f;
        char row[64];
        for (uint32_t i = 0; i < content.high_score_count && i < 10; ++i) {
            const auto& hs = content.high_scores[i];
            const bool top = i == 0;
            // Sanitize per glyph: any control byte (embedded NUL
            // truncates the row's std::string, others corrupt the
            // bitmap-font row) renders as a space — the score always
            // shows (cycle-30/31 review).
            const auto glyph = [](char c) {
                return static_cast<unsigned char>(c) >= 0x20u ? c : ' ';
            };
            const char clean[3] = {
                glyph(hs.initials[0]),
                glyph(hs.initials[1]),
                glyph(hs.initials[2]),
            };
            std::snprintf(row,
                          sizeof(row),
                          "%u %c%c%c  %s",
                          i + 1,
                          clean[0],
                          clean[1],
                          clean[2],
                          game::format_score(hs.score).c_str());
            if (top) {
                text(font, 24.0f, y, row, kActiveR, kActiveG, kActiveB, out);
            } else {
                text(font, 24.0f, y, row, kDimR, kDimG, kDimB, out);
            }
            y += 30.0f;
        }
        if (content.high_score_count == 0) {
            text(font, 24.0f, 60.0f, "NO SCORES YET", kDimR, kDimG, kDimB, out);
        }
        return;
    }

    // §9.2 A — score cards: as many as players, arranged in a 2-wide
    // grid; the active player's card is larger and highlighted.
    const int n = std::max(1, std::min(4, content.player_count));
    const float card_w = n <= 1 ? (W - 48.0f) : (W - 48.0f - 16.0f) * 0.5f;
    const float card_h = 150.0f;
    for (int i = 0; i < n; ++i) {
        const float col = float(i % 2);
        const float cx = 24.0f + card_w * 0.5f + col * (card_w + 16.0f);
        const float cy = 24.0f + card_h * 0.5f + float(i / 2) * (card_h + 12.0f);
        const bool active = (i + 1) == content.current_player;
        const float scale = active ? 1.0f : 0.92f;
        quad(cx, cy, card_w * 0.5f * scale, card_h * 0.5f * scale, 0.09f, 0.10f, 0.14f, 1.0f, out);
        char label[8];
        std::snprintf(label, sizeof(label), "P%d", i + 1);
        const float lr = active ? kActiveR : kDimR;
        const float lg = active ? kActiveG : kDimG;
        const float lb = active ? kActiveB : kDimB;
        text(font,
             cx - card_w * 0.5f * scale + 10.0f,
             cy - card_h * 0.5f * scale + 10.0f,
             label,
             lr,
             lg,
             lb,
             out);
        text(font,
             cx - card_w * 0.5f * scale + 10.0f,
             cy - 10.0f,
             game::format_score(content.scores[size_t(i)]),
             active ? kTextR : kDimR,
             active ? kTextG : kDimG,
             active ? kTextB : kDimB,
             out);
    }

    // §9.2 B — status band: ball number + player count.
    char band[64];
    std::snprintf(band, sizeof(band), "BALL %d   PLAYERS %d", content.ball_number, n);
    text(font, 24.0f, H - 84.0f, band, kDimR, kDimG, kDimB, out);

    // §9.2 C — mode/timer slot: the model layout name (M14 owns real
    // mode timers).
    const char* layout_name = model.layout == 1 ? "MODE" : model.layout == 2 ? "CELEBRATE" : "";

    // §9.2 D — message ticker: the BackglassModel message when present
    // (framework messages preempt script ones upstream of the model).
    // Clamp defensively at the layout boundary: a caller-supplied
    // message_len past the buffer truncates, never reads past
    // kMessageCap (cycle-15 review).
    const uint32_t msg_len =
        std::min(uint32_t(model.message_len), uint32_t(sizeof(model.message) - 1));
    if (msg_len > 0) {
        const uint32_t style =
            uint32_t(model.message_style) < 4 ? uint32_t(model.message_style) : 0;
        quad(W * 0.5f, H - 40.0f, W * 0.5f - 16.0f, 20.0f, 0.10f, 0.11f, 0.16f, 1.0f, out);
        text(font,
             28.0f,
             H - 34.0f,
             std::string(model.message, msg_len),
             kMsgStyle[style][0],
             kMsgStyle[style][1],
             kMsgStyle[style][2],
             out);
    } else if (layout_name[0] != '\0') {
        text(font,
             28.0f,
             H - 34.0f,
             layout_name,
             kMsgStyle[1][0],
             kMsgStyle[1][1],
             kMsgStyle[1][2],
             out);
    }
}

} // namespace tb::render
