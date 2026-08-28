#pragma once

#include <string>
#include <vector>

// Display enumeration results and the assignment heuristic
// (07-displays.md §2–§5, milestone M12). Pure data + pure functions —
// no SDL types here, so detection is CI-testable without displays;
// the SDL fill-in lives in display_detect_sdl.cpp.
namespace tb::platform {

struct DisplayInfo {
    int index = 0;
    int w = 0;
    int h = 0;
    float refresh_hz = 0.0f; // 0/unknown -> treated as 60 by scoring
    std::string name;        // e.g. "SAMSUNG 32in TV"
};

// min(w,h)/max(w,h) in (0,1]; 1.0 = square (07 §1).
float squareness(const DisplayInfo& d);

// displays.json role blocks (07 §5).
struct RoleConfig {
    std::string match = "auto";    // "auto" | "name:<glob>" | "index:<n>"
    std::string rotation = "auto"; // "auto" | "0"|"90"|"180"|"270"
    bool enabled = true;           // backglass only
};

struct LastAuto {
    bool present = false;
    std::string playfield;
    std::string backglass; // empty = none
};

struct DisplaysConfig {
    RoleConfig playfield;
    RoleConfig backglass;
    LastAuto last_auto;
};

struct Assignment {
    int playfield = -1;  // index into the input list; -1 = none
    int backglass = -1;  // -1 = none
    int pf_rotation = 0; // degrees CCW, 0/90/180/270
    int bg_rotation = 0;
    std::vector<std::string> warnings;
    bool stability_reused = false; // T14 probe: last_auto path taken
};

// The binding heuristic (07 §3): explicit config beats heuristics, the
// last_auto stability path, portrait/landscape pools with lexicographic
// argmax keys, squarest-backglass, and §3's auto_pf_rotation. Fully
// deterministic for a given input list.
Assignment detect(const std::vector<DisplayInfo>& displays, const DisplaysConfig& cfg);

// `match` resolution (07 §3): "auto" -> -1; "index:<n>" -> that 0-based
// position (clamped to -1 when out of range); "name:<glob>" ->
// case-sensitive glob (* and ?) over names, lowest index wins, and
// `ambiguous` is set when several displays match. Unknown forms -> -1.
int resolve_match(const std::string& match,
                  const std::vector<DisplayInfo>& displays,
                  bool* ambiguous = nullptr);

// displays.json (07 §5): missing file -> all-"auto" defaults + false.
// Corrupt file -> false + defaults (the caller logs; the run continues
// on heuristics rather than dying over a config file).
struct DisplaysFileResult {
    bool loaded = false; // false: missing or corrupt
    bool corrupt = false;
    DisplaysConfig cfg;
};

DisplaysFileResult load_displays_json(const std::string& text);

// Serializes the config INCLUDING last_auto; caller writes it
// crash-safe to the pref path (never to a --display-config file).
std::string save_displays_json(const DisplaysConfig& cfg);

} // namespace tb::platform
