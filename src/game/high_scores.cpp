#include "game/high_scores.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tb::game {

namespace {
using json = nlohmann::json;

// Crash-safe write (05-engine-core.md §11.2): tmp + fsync + rename
// (+ dir fsync on POSIX) — a crash during write never loses the
// existing top 10.
bool write_crash_safe(const std::filesystem::path& path, const std::string& text) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
        if (f == nullptr) {
            TB_LOG_ERROR("game", "scores save: cannot open {}", tmp.string());
            return false;
        }
        const bool ok =
            std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
        if (ok) {
            _commit(_fileno(f));
        }
#else
        if (ok) {
            fsync(fileno(f));
        }
#endif
        std::fclose(f);
        if (!ok) {
            TB_LOG_ERROR("game", "scores save: short write to {}", tmp.string());
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        TB_LOG_ERROR("game", "scores save: rename failed ({})", ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
#if !defined(_WIN32)
    if (std::filesystem::path dir = path.parent_path(); !dir.empty()) {
        if (std::FILE* d = std::fopen(dir.string().c_str(), "rb"); d != nullptr) {
            fsync(fileno(d));
            std::fclose(d);
        }
    }
#endif
    return true;
}

bool valid_initials(const std::array<char, 3>& ini) {
    auto glyph_ok = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
    };
    return glyph_ok(ini[0]) && glyph_ok(ini[1]) && glyph_ok(ini[2]);
}

} // namespace

bool HighScoreTable::load(const std::filesystem::path& path) {
    entries_.clear();
    std::ifstream in(path);
    if (!in.good()) {
        return false; // missing: caller seeds (§7)
    }
    try {
        json doc = json::parse(in);
        if (doc.value("version", 0) != 1 || !doc.contains("entries") ||
            !doc.at("entries").is_array()) {
            return false; // corrupt: caller re-seeds
        }
        for (const json& row : doc.at("entries")) {
            if (!row.is_object() || !row.contains("initials") || !row.contains("score") ||
                !row.at("initials").is_string() || !row.at("score").is_number_unsigned()) {
                return false;
            }
            HighScoreEntry e;
            const std::string ini = row.at("initials").get<std::string>();
            if (ini.size() != 3) {
                return false;
            }
            std::copy(ini.begin(), ini.end(), e.initials.begin());
            e.score = row.at("score").get<uint64_t>();
            if (row.contains("date") && row.at("date").is_string()) {
                e.date = row.at("date").get<std::string>();
            }
            if (!valid_initials(e.initials) || e.score == 0) {
                return false;
            }
            entries_.push_back(e);
        }
        if (entries_.size() > kMaxEntries) {
            return false;
        }
        return true;
    } catch (const json::exception&) {
        return false; // corrupt
    }
}

void HighScoreTable::seed_defaults(const std::vector<table::DefaultScore>& defaults,
                                   const std::string& date) {
    entries_.clear();
    for (const table::DefaultScore& d : defaults) {
        HighScoreEntry e;
        e.initials = {d.initials[0], d.initials[1], d.initials[2]};
        e.score = d.score;
        e.date = date;
        entries_.push_back(e);
    }
}

bool HighScoreTable::qualifies(uint64_t score) const {
    if (entries_.size() < kMaxEntries) {
        return true;
    }
    // Ties insert BELOW existing equal scores: strictly-greater against
    // the last row.
    return score > entries_.back().score;
}

int HighScoreTable::insert(const HighScoreEntry& entry) {
    if (!qualifies(entry.score)) {
        return 0;
    }
    size_t at = entries_.size();
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entry.score > entries_[i].score) {
            at = i;
            break;
        }
    }
    entries_.insert(entries_.begin() + std::ptrdiff_t(at), entry);
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
    }
    return int(at) + 1;
}

bool HighScoreTable::save(const std::filesystem::path& path, const std::string& slug) const {
    json doc;
    doc["version"] = 1;
    doc["table"] = slug;
    json rows = json::array();
    for (const HighScoreEntry& e : entries_) {
        rows.push_back(json{
            {"initials", std::string(e.initials.begin(), e.initials.end())},
            {"score", e.score},
            {"date", e.date},
        });
    }
    doc["entries"] = std::move(rows);
    return write_crash_safe(path, doc.dump(2) + "\n");
}

} // namespace tb::game
