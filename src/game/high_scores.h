#pragma once

#include "table/table_types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tb::game {

// One top-10 row (11-game-framework.md §7).
struct HighScoreEntry {
    std::array<char, 3> initials{'A', 'A', 'A'};
    uint64_t score = 0;
    std::string date; // YYYY-MM-DD local date, stamped at write time

    bool operator==(const HighScoreEntry&) const = default;
};

// Per-table top 10. File layout (§7):
//   { "version": 1, "table": "<slug>", "entries": [ {initials, score,
//   date} ] }
// written with the crash-safe temp + rename rule (05-engine-core.md
// §11.2). A table that declares no meta.default_scores starts EMPTY —
// there is no built-in ladder (§7, binding).
class HighScoreTable {
public:
    static constexpr size_t kMaxEntries = 10;

    // Reads <path>. Returns false when the file is missing or corrupt;
    // the caller then seeds from meta.default_scores (or leaves the
    // list empty when the table declares none).
    bool load(const std::filesystem::path& path);

    // Seeds exactly what seeding would write (§7): the declared
    // defaults with `date`, or nothing for an empty list.
    void seed_defaults(const std::vector<table::DefaultScore>& defaults, const std::string& date);

    // §7: qualifies if < 10 entries or the score beats the 10th; ties
    // insert below existing equal scores.
    bool qualifies(uint64_t score) const;

    // Inserts sorted (ties below equals); returns the 1-based rank, or
    // 0 when the entry did not qualify. Truncates to 10 rows.
    int insert(const HighScoreEntry& entry);

    // Crash-safe write: temp file + fsync + rename + dir fsync.
    void save(const std::filesystem::path& path, const std::string& slug) const;

    const std::vector<HighScoreEntry>& entries() const { return entries_; }

    void clear() { entries_.clear(); }

private:
    std::vector<HighScoreEntry> entries_;
};

} // namespace tb::game
