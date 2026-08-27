#pragma once

#include <array>
#include <cstdint>

namespace tb::game {

// HighScoreEntry glyph-ring model (11-game-framework.md §7). Three
// slots, cursor on slot 1, every slot pre-showing 'A'. The ring is
// A..Z 0..9 space then the backspace glyph (represented as '<').
// Hold-to-repeat is driven by the caller (repeat ticks); the model is
// pure state.
class InitialsEntry {
public:
    static constexpr int kSlots = 3;
    // Ring: 26 letters, 10 digits, space, backspace = 38 glyphs.
    static constexpr int kRingSize = 38;
    static constexpr int kBackspaceIndex = 37;

    void begin() {
        slots_ = {'A', 'A', 'A'};
        ring_pos_ = 0;
        cursor_ = 0;
        idle_ticks_ = 0;
        done_ = false;
    }

    // One sim tick of the entry state machine.
    void tick() {
        if (done_) {
            return;
        }
        if (++idle_ticks_ >= 60'000) {
            // §7: 60 s without input commits the displayed glyphs.
            done_ = true;
        }
    }

    void next_glyph() {
        touch();
        ring_pos_ = (ring_pos_ + 1) % kRingSize;
        slots_[size_t(cursor_)] = glyph_at(ring_pos_);
    }

    void prev_glyph() {
        touch();
        ring_pos_ = (ring_pos_ + kRingSize - 1) % kRingSize;
        slots_[size_t(cursor_)] = glyph_at(ring_pos_);
    }

    // Start: confirm the slot and advance. Confirming slot 3 commits
    // (the ring never rests on backspace — selecting '<' + Start is
    // backspace, handled by try_confirm below).
    void confirm() {
        touch();
        if (cursor_ < kSlots - 1) {
            ++cursor_;
            ring_pos_ = ring_of(slots_[size_t(cursor_)]);
        } else {
            done_ = true;
        }
    }

    // Plunger key: move back one slot. Returns true while there is a
    // slot to move back to (at slot 0 the caller treats it as a no-op).
    bool backspace() {
        touch();
        if (cursor_ > 0) {
            --cursor_;
            ring_pos_ = ring_of(slots_[size_t(cursor_)]);
            return true;
        }
        return false;
    }

    bool done() const { return done_; }

    int cursor() const { return cursor_; }

    // Committable initials: '<' is a control glyph — a slot resting on
    // it at the 60 s timeout commits as a space (§7: committed glyphs
    // are A–Z 0–9 space only).
    std::array<char, 3> initials() const {
        std::array<char, 3> out = slots_;
        for (char& c : out) {
            if (c == '<') {
                c = ' ';
            }
        }
        return out;
    }

    static char glyph_at(int ring_pos) {
        if (ring_pos < 26) {
            return char('A' + ring_pos);
        }
        if (ring_pos < 36) {
            return char('0' + (ring_pos - 26));
        }
        if (ring_pos == 36) {
            return ' ';
        }
        return '<'; // backspace glyph
    }

    static int ring_of(char c) {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= '0' && c <= '9') {
            return 26 + (c - '0');
        }
        if (c == ' ') {
            return 36;
        }
        return kBackspaceIndex;
    }

private:
    void touch() { idle_ticks_ = 0; }

    std::array<char, 3> slots_{'A', 'A', 'A'};
    int cursor_ = 0;
    int ring_pos_ = 0;
    uint32_t idle_ticks_ = 0;
    bool done_ = false;
};

} // namespace tb::game
