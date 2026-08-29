#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// settings.json (05-engine-core.md §11). This struct mirrors the §11.1
// schema; unknown keys in the file are preserved on save. Out-of-range
// values are clamped to range and logged at warn.
namespace tb {

struct Settings {
    // video
    std::string present_mode = "auto"; // "auto" | "mailbox" | "vsync"
    int max_fps = 0;                   // 0 = match display, -1 = uncapped
    float brightness = 1.0f;           // 0.5 .. 1.5

    // render (consumed from M13)
    bool bloom_enabled = true;
    float bloom_threshold = 1.0f;
    float bloom_knee = 0.5f;
    float bloom_strength = 0.6f; // render.bloom_strength, 0-2 (06 §12.5)
    bool crt = false;            // render.crt, user-only (13 §10)

    // audio — volumes 0..100, gain = (v/100)^2 (12-audio.md §3.1)
    int audio_master = 80;
    int audio_sfx = 100;
    int audio_music = 60;
    int audio_ui = 80;
    int audio_period_frames = 0;

    // input — action -> scancode names (§9.1); multiple names per action
    // are allowed (§11.1). The two scalars below ride the same block.
    std::array<std::vector<std::string>, 10> bindings;
    float plunger_max_pull_s = 1.5f; // 0.5 .. 3.0
    int nudge_level = 2;             // 1 | 2 | 3

    // game
    int balls_per_game = 3;    // 3 or 5 only
    int tilt_warnings = 2;     // 1..3
    int ball_save_seconds = 8; // 0..15
    std::string replay_award = "extra_ball";
    std::string last_table = "neon-drift";

    // accessibility
    bool reduce_flashing = false;
    bool ball_outline = false;
    bool screen_shake = true;

    // Default bindings per 05 §9.1, one action-indexed list each.
    static Settings defaults();

    // Loads path; on parse error renames to <path>.bad and returns defaults
    // with a warn (fallback, not failure).
    static Settings load(const std::filesystem::path& path);

    // Crash-safe write: tmp + fsync + rename + dir fsync (§11.2). Unknown
    // keys previously present in the file survive the round trip.
    bool save(const std::filesystem::path& path) const;
};

} // namespace tb
