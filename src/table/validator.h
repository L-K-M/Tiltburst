#pragma once

// tb_validate's core (09-table-format.md §8 catalog, §10 diagnostics).
// Pure: loads the pack, runs every V-rule the in-game loader does not
// already own, and returns diagnostics — never throws. The loader
// itself stays the owner of V000–V031's schema-level checks that fire
// during load; validate_pack surfaces its first error as one
// diagnostic and stops (fix the first error; geometry cascades).

#include <filesystem>
#include <string>
#include <vector>

namespace tb::table {

struct ValidationDiag {
    std::string file;     // pack-relative ("table.json")
    std::string pointer;  // JSON pointer ("" for pack-level findings)
    std::string code;     // "V0nn"
    std::string severity; // "error" | "warning"
    std::string message;

    // 09 §10 line format: file:pointer [Vnnn][severity] message.
    std::string line() const;
    // 09 §10 --json object shape.
    std::string json() const;
};

// Severity per 09 §8 is fixed by code; helper for tests/tools.
const char* diag_severity(const std::string& code);

// Runs the full catalog over <dir>/ (table.json required; rules.lua,
// art.json, audio.json when present). Diagnostics are unordered.
std::vector<ValidationDiag> validate_pack(const std::filesystem::path& dir);

} // namespace tb::table
