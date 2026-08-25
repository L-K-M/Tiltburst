#pragma once

#include <string>

// Application lifecycle (05-engine-core.md §1–§2).
namespace tb::app {

inline constexpr char kUsage[] =
    "tiltburst - digital pinball\n"
    "\n"
    "Usage:\n"
    "  tiltburst --version                 Print the version and exit.\n"
    "  tiltburst [--windowed WxH] [--table slug] [--dev] [--headless]\n"
    "  tiltburst --render-smoke --frames N --screenshot-dir DIR\n"
    "\n"
    "Unknown flags, malformed values, or contradictory combinations exit 2\n"
    "(05-engine-core.md §2.1).\n";

struct CliOptions {
    bool version = false;
    bool headless = false;
    bool dev = false;
    bool render_smoke = false;
    int frames = 120; // --render-smoke frame count
    int window_w = 540;
    int window_h = 1080;
    std::string screenshot_dir;
    std::string table;

    // Set when parsing fails: reason filled, exit_code == 2 (§2.1).
    std::string error;
};

// Pure string parsing, no SDL calls (§1 step 1). --version short-circuits
// the rest of the command line and is reported via `version`.
CliOptions parse_cli(int argc, char** argv);

// §1 steps 2–13 + main loop + shutdown. Never called with a parse error.
int run(const CliOptions& cli);

} // namespace tb::app
