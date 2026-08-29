#include "platform/display_detect.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace tb::platform {

namespace {

using json = nlohmann::ordered_json;

// --- glob: '*' and '?' only, case-sensitive (07 §3) ---
bool glob_match(const char* pattern, const char* text) {
    // Iterative backtracking matcher (classic two-pointer with wildcard
    // restart); patterns here are short display names.
    const char* p = pattern;
    const char* t = text;
    const char* star = nullptr;
    const char* star_t = nullptr;
    while (*t != '\0') {
        if (*p == '?' || *p == *t) {
            ++p;
            ++t;
        } else if (*p == '*') {
            star = p++;
            star_t = t;
        } else if (star != nullptr) {
            p = star + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        ++p;
    }
    return *p == '\0';
}

float effective_hz(const DisplayInfo& d) {
    return d.refresh_hz > 0.0f ? d.refresh_hz : 60.0f;
}

// §3 step 3: playfield pool key (higher wins, lexicographic).
bool pf_beats(const DisplayInfo& a, const DisplayInfo& b) {
    const long long area_a = static_cast<long long>(a.w) * a.h;
    const long long area_b = static_cast<long long>(b.w) * b.h;
    if (area_a != area_b) {
        return area_a > area_b;
    }
    if (effective_hz(a) != effective_hz(b)) {
        return effective_hz(a) > effective_hz(b);
    }
    return a.index < b.index; // -index: lower index wins
}

// §3 step 3: backglass key — EXACTLY (squareness, w*h, -index);
// deliberately NOT reusing pf_beats (which folds in refresh_hz — the
// backglass key has no Hz term; cycle-8 review).
bool bg_beats(const DisplayInfo& a, const DisplayInfo& b) {
    const float sq_a = squareness(a);
    const float sq_b = squareness(b);
    if (sq_a != sq_b) {
        return sq_a > sq_b;
    }
    const long long area_a = static_cast<long long>(a.w) * a.h;
    const long long area_b = static_cast<long long>(b.w) * b.h;
    if (area_a != area_b) {
        return area_a > area_b;
    }
    return a.index < b.index;
}

// §3 step 4: the cabinet-vs-desktop rotation call.
int auto_pf_rotation(const DisplayInfo& pf, int bg, const std::vector<DisplayInfo>& ds) {
    if (pf.h >= pf.w) {
        return 0; // reported portrait: already upright
    }
    if (bg != -1) {
        // Scan, never index by value: the id<->position invariant is a
        // producer convention, not a type guarantee (cycle-24).
        for (const DisplayInfo& d : ds) {
            if (d.index == bg && squareness(d) >= 0.70f) {
                return 90; // cabinet assumption; flip -> 270 via config
            }
        }
    }
    return 0; // desktop: render portrait, pillarboxed
}

int parse_rotation(const std::string& rot) {
    if (rot == "90") {
        return 90;
    }
    if (rot == "180") {
        return 180;
    }
    if (rot == "270") {
        return 270;
    }
    return 0; // "auto", "0", anything else -> 0 at this layer
}

} // namespace

float squareness(const DisplayInfo& d) {
    const int lo = std::min(d.w, d.h);
    const int hi = std::max(d.w, d.h);
    return hi > 0 ? float(lo) / float(hi) : 0.0f;
}

int resolve_match(const std::string& match,
                  const std::vector<DisplayInfo>& displays,
                  bool* ambiguous) {
    if (ambiguous != nullptr) {
        *ambiguous = false;
    }
    if (match == "auto" || match.empty()) {
        return -1;
    }
    if (match.rfind("index:", 0) == 0) {
        const std::string digits = match.substr(6);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) {
                return c >= '0' && c <= '9';
            })) {
            return -1; // "index:abc"/"index: 2" must not silently bind 0
        }
        // strtoll (not strtol/atoi): atoi on out-of-range digit
        // strings is UB, and on LLP64 Windows long == int, where
        // strtol's overflow behavior is indistinguishable from a valid
        // end-of-parse (cycle-10 review). The 64-bit form sets errno.
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(digits.c_str(), &end, 10);
        // end == start rejects a no-conversion result ("index:")
        // unconditionally — not by the empty-digits check's ordering.
        if (errno == ERANGE || end == nullptr || end == digits.c_str() || *end != '\0' || v < 0 ||
            v > static_cast<long long>(std::numeric_limits<int>::max())) {
            return -1;
        }
        const int n = int(v);
        return size_t(n) < displays.size() ? n : -1;
    }
    if (match.rfind("name:", 0) == 0) {
        const std::string pattern = match.substr(5);
        int found = -1;
        int hits = 0;
        for (const DisplayInfo& d : displays) {
            if (glob_match(pattern.c_str(), d.name.c_str())) {
                if (found < 0) {
                    found = d.index;
                }
                ++hits;
            }
        }
        if (found >= 0 && hits > 1 && ambiguous != nullptr) {
            *ambiguous = true; // duplicate-name monitors are common
        }
        return found;
    }
    return -1;
}

Assignment detect(const std::vector<DisplayInfo>& displays, const DisplaysConfig& cfg) {
    Assignment a;
    if (displays.empty()) {
        return a; // caller: fatal unless --headless (T11)
    }

    // --- 1. explicit config beats heuristics, per role independently ---
    bool pf_ambiguous = false;
    bool bg_ambiguous = false;
    int pf = resolve_match(cfg.playfield.match, displays, &pf_ambiguous);
    int bg =
        cfg.backglass.enabled ? resolve_match(cfg.backglass.match, displays, &bg_ambiguous) : -1;
    if (pf_ambiguous) {
        a.warnings.push_back("playfield name match '" + cfg.playfield.match +
                             "' is ambiguous; using the lowest index");
    }
    if (bg_ambiguous) {
        a.warnings.push_back("backglass name match '" + cfg.backglass.match +
                             "' is ambiguous; using the lowest index");
    }
    if (!cfg.playfield.match.empty() && cfg.playfield.match != "auto" && pf == -1) {
        a.warnings.push_back("playfield match '" + cfg.playfield.match +
                             "' not found; using heuristics");
    }
    if (cfg.backglass.enabled && !cfg.backglass.match.empty() && cfg.backglass.match != "auto" &&
        bg == -1) {
        a.warnings.push_back("backglass match '" + cfg.backglass.match +
                             "' not found; using heuristics");
    }
    bool bg_dropped = false; // same-display collision: FINAL (§3 step 1)
    if (pf != -1 && pf == bg) {
        a.warnings.push_back("playfield and backglass match the same display; backglass dropped");
        bg = -1;
        bg_dropped = true;
    }

    // --- 2. stability: reuse last auto assignment when nothing changed ---
    // Stability protects the PLAYFIELD across runs even when the
    // backglass is disabled — only the backglass half of the reuse is
    // suppressed below (cycle-4 review).
    if (pf == -1 && (cfg.backglass.match.empty() || cfg.backglass.match == "auto") &&
        cfg.last_auto.present) {
        // Same display-name SET and every last_auto name resolves uniquely.
        std::vector<std::string> names;
        for (const DisplayInfo& d : displays) {
            names.push_back(d.name);
        }
        std::vector<std::string> last_names;
        if (!cfg.last_auto.playfield.empty()) {
            last_names.push_back(cfg.last_auto.playfield);
        }
        if (!cfg.last_auto.backglass.empty()) {
            last_names.push_back(cfg.last_auto.backglass);
        }
        // §3 step 2: SET equality, BOTH directions — a newly attached
        // or removed display breaks the reuse (subset-only kept a
        // no-backglass result forever; cycle-1 review).
        auto set_of = [](std::vector<std::string> v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            return v;
        };
        const bool all_present = !last_names.empty() && set_of(names) == set_of(last_names);
        if (all_present) {
            // Resolve by name against the CURRENT indices, capturing
            // the elements directly — never re-index the vector by a
            // role index value (cycle-24 review).
            int pf_by_name = -1;
            int bg_by_name = -1;
            const DisplayInfo* pf_disp = nullptr;
            for (const DisplayInfo& d : displays) {
                if (!cfg.last_auto.playfield.empty() && d.name == cfg.last_auto.playfield &&
                    pf_by_name == -1) {
                    pf_by_name = d.index;
                    pf_disp = &d;
                }
                if (!cfg.last_auto.backglass.empty() && d.name == cfg.last_auto.backglass &&
                    bg_by_name == -1) {
                    bg_by_name = d.index;
                }
            }
            // Fall through to the heuristic when an ENABLED backglass
            // has no recording (duplicate-name rigs where set equality
            // holds despite a second display) — cycle-29 review.
            if (pf_by_name != -1 && pf_by_name != bg_by_name &&
                (!cfg.backglass.enabled || bg_by_name != -1)) {
                a.playfield = pf_by_name;
                a.backglass = cfg.backglass.enabled ? bg_by_name : -1;
                a.stability_reused = true;
                a.pf_rotation =
                    cfg.playfield.rotation != "auto"
                        ? parse_rotation(cfg.playfield.rotation)
                        : (pf_disp != nullptr ? auto_pf_rotation(*pf_disp, a.backglass, displays)
                                              : 0);
                a.bg_rotation = parse_rotation(cfg.backglass.rotation);
                return a;
            }
        }
    }

    // --- 3. heuristic scoring ---
    // An explicitly matched backglass owns its display: the heuristic
    // playfield must not land on it (cycle-24 review).
    const int pf_pool_exclusion = (pf == -1 && bg != -1) ? bg : -1;
    std::vector<const DisplayInfo*> portrait;
    std::vector<const DisplayInfo*> landscape;
    for (const DisplayInfo& d : displays) {
        if (d.index == pf_pool_exclusion) {
            continue;
        }
        if (float(d.h) / float(d.w) >= 1.4f) {
            portrait.push_back(&d);
        } else if (float(d.w) / float(d.h) >= 1.4f) {
            landscape.push_back(&d);
        }
    }
    if (pf == -1) {
        // Portrait else landscape else everything (the neither-pool
        // case: near-square displays like 5:4 panels).
        const std::vector<const DisplayInfo*>* pool = &portrait;
        if (pool->empty()) {
            pool = &landscape;
        }
        std::vector<const DisplayInfo*> everything;
        if (pool->empty()) {
            for (const DisplayInfo& d : displays) {
                if (d.index != pf_pool_exclusion) {
                    everything.push_back(&d);
                }
            }
            if (everything.empty()) {
                // Degenerate: the only display IS the explicit
                // backglass (single-monitor rig). Degrade to the
                // pre-exclusion pool rather than an empty one
                // (cycle-25 review) — the same-display drop in step 1
                // then resolves the collision.
                for (const DisplayInfo& d : displays) {
                    everything.push_back(&d);
                }
            }
            pool = &everything;
        }
        const DisplayInfo* best = nullptr;
        for (const DisplayInfo* d : *pool) {
            if (best == nullptr || pf_beats(*d, *best)) {
                best = d;
            }
        }
        pf = best != nullptr ? best->index : -1;
    }
    // A failed explicit match falls through to heuristics (the warning
    // above promises it); a same-display collision drop is deliberate
    // and must NOT be re-picked (cycle-1 regression).
    if (bg == -1 && !bg_dropped && cfg.backglass.enabled) {
        const DisplayInfo* best = nullptr;
        for (const DisplayInfo& d : displays) {
            if (d.index == pf) {
                continue;
            }
            if (best == nullptr || bg_beats(d, *best)) {
                best = &d;
            }
        }
        bg = best != nullptr ? best->index : -1;
    }

    // Post-heuristic collision: the heuristic playfield can land on
    // the explicitly matched backglass (single-display degradation);
    // the playfield wins and the backglass drops — the same resolution
    // the explicit-explicit check applies in step 1, re-run here
    // because pf is only known AFTER the heuristic.
    if (pf != -1 && pf == bg) {
        a.warnings.push_back("playfield and backglass match the same display; backglass dropped");
        bg = -1;
    }

    // --- 4. rotation resolution ---
    a.playfield = pf;
    a.backglass = bg;
    a.pf_rotation =
        cfg.playfield.rotation != "auto" ? parse_rotation(cfg.playfield.rotation) : ([&] {
            for (const DisplayInfo& d : displays) {
                if (d.index == pf) {
                    return auto_pf_rotation(d, bg, displays);
                }
            }
            return 0;
        })();
    a.bg_rotation = parse_rotation(cfg.backglass.rotation);
    return a;
}

// ---- displays.json (07 §5) ----

namespace {

bool parse_role(const json& obj, RoleConfig& out, const std::string& pointer) {
    if (!obj.is_object()) {
        return false;
    }
    if (auto it = obj.find("match"); it != obj.end() && it->is_string()) {
        out.match = it->get<std::string>();
    }
    if (auto it = obj.find("rotation"); it != obj.end() && it->is_string()) {
        const std::string r = it->get<std::string>();
        if (r != "auto" && r != "0" && r != "90" && r != "180" && r != "270") {
            (void)pointer;
            return false;
        }
        out.rotation = r;
    }
    if (auto it = obj.find("enabled"); it != obj.end() && it->is_boolean()) {
        out.enabled = it->get<bool>();
    }
    return true;
}

} // namespace

DisplaysFileResult load_displays_json(const std::string& text) {
    DisplaysFileResult res;
    if (text.empty()) {
        return res; // missing file
    }
    json doc;
    try {
        doc = json::parse(text, nullptr, true, true); // comments allowed
    } catch (const json::parse_error&) {
        res.corrupt = true;
        return res;
    }
    if (!doc.is_object()) {
        res.corrupt = true;
        return res;
    }
    if (auto it = doc.find("version"); it != doc.end()) {
        // ANY present version must be the integer 1 — a string or float
        // form is an unknown schema: refuse, not guess.
        // 64-bit compare first: a huge integer must not narrow to 1.
        if (!it->is_number_integer() || it->get<int64_t>() != 1) {
            res.corrupt = true;
            return res;
        }
    }
    DisplaysConfig cfg;
    if (auto it = doc.find("playfield"); it != doc.end()) {
        if (!parse_role(*it, cfg.playfield, "/playfield")) {
            res.corrupt = true;
            return res;
        }
    }
    if (auto it = doc.find("backglass"); it != doc.end()) {
        if (!parse_role(*it, cfg.backglass, "/backglass")) {
            res.corrupt = true;
            return res;
        }
    }
    if (auto it = doc.find("last_auto"); it != doc.end() && it->is_object()) {
        cfg.last_auto.present = true;
        if (auto p = it->find("playfield"); p != it->end() && p->is_string()) {
            cfg.last_auto.playfield = p->get<std::string>();
        }
        if (auto b = it->find("backglass"); b != it->end() && b->is_string()) {
            cfg.last_auto.backglass = b->get<std::string>();
        }
    }
    res.loaded = true;
    res.cfg = cfg;
    return res;
}

std::string save_displays_json(const DisplaysConfig& cfg) {
    json doc;
    doc["version"] = 1;
    doc["playfield"] = {{"match", cfg.playfield.match}, {"rotation", cfg.playfield.rotation}};
    doc["backglass"] = {{"match", cfg.backglass.match},
                        {"rotation", cfg.backglass.rotation},
                        {"enabled", cfg.backglass.enabled}};
    json last = json::object();
    last["playfield"] = cfg.last_auto.present ? cfg.last_auto.playfield : "";
    last["backglass"] =
        cfg.last_auto.present && !cfg.last_auto.backglass.empty() ? cfg.last_auto.backglass : "";
    doc["last_auto"] = std::move(last);
    return doc.dump(2) + "\n";
}

} // namespace tb::platform
