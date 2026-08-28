#include "platform/display_detect.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// §3 step 3: backglass key (squareness, then area, then index).
bool bg_beats(const DisplayInfo& a, const DisplayInfo& b) {
    const float sq_a = squareness(a);
    const float sq_b = squareness(b);
    if (sq_a != sq_b) {
        return sq_a > sq_b;
    }
    return pf_beats(a, b);
}

// §3 step 4: the cabinet-vs-desktop rotation call.
int auto_pf_rotation(const DisplayInfo& pf, int bg, const std::vector<DisplayInfo>& ds) {
    if (pf.h >= pf.w) {
        return 0; // reported portrait: already upright
    }
    if (bg != -1 && bg >= 0 && size_t(bg) < ds.size() && squareness(ds[size_t(bg)]) >= 0.70f) {
        return 90; // cabinet assumption; flip -> 270 via config
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
        const int n = std::atoi(match.c_str() + 6);
        return n >= 0 && size_t(n) < displays.size() ? n : -1;
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
    int pf = resolve_match(cfg.playfield.match, displays);
    int bg = cfg.backglass.enabled ? resolve_match(cfg.backglass.match, displays) : -1;
    if (cfg.playfield.match != "auto" && pf == -1) {
        a.warnings.push_back("playfield match '" + cfg.playfield.match +
                             "' not found; using heuristics");
    }
    if (cfg.backglass.enabled && cfg.backglass.match != "auto" && bg == -1) {
        a.warnings.push_back("backglass match '" + cfg.backglass.match +
                             "' not found; using heuristics");
    }
    if (pf != -1 && pf == bg) {
        a.warnings.push_back("playfield and backglass match the same display; backglass dropped");
        bg = -1;
    }

    // --- 2. stability: reuse last auto assignment when nothing changed ---
    if (pf == -1 && cfg.backglass.match == "auto" && cfg.last_auto.present) {
        // Same display-name SET and every last_auto name resolves uniquely.
        std::vector<std::string> names;
        for (const DisplayInfo& d : displays) {
            names.push_back(d.name);
        }
        std::vector<std::string> last_names = names; // rebuild from cfg
        last_names.clear();
        if (!cfg.last_auto.playfield.empty()) {
            last_names.push_back(cfg.last_auto.playfield);
        }
        if (!cfg.last_auto.backglass.empty()) {
            last_names.push_back(cfg.last_auto.backglass);
        }
        // All last_auto entries must exist in the current set.
        bool all_present = true;
        for (const std::string& want : last_names) {
            const bool found = std::any_of(
                names.begin(), names.end(), [&](const std::string& n) { return n == want; });
            if (!found) {
                all_present = false;
                break;
            }
        }
        if (all_present && !last_names.empty()) {
            // Resolve by name against the CURRENT indices.
            int pf_by_name = -1;
            int bg_by_name = -1;
            for (const DisplayInfo& d : displays) {
                if (!cfg.last_auto.playfield.empty() && d.name == cfg.last_auto.playfield &&
                    pf_by_name == -1) {
                    pf_by_name = d.index;
                }
                if (!cfg.last_auto.backglass.empty() && d.name == cfg.last_auto.backglass &&
                    bg_by_name == -1) {
                    bg_by_name = d.index;
                }
            }
            if (pf_by_name != -1 && pf_by_name != bg_by_name) {
                a.playfield = pf_by_name;
                a.backglass = bg_by_name;
                a.stability_reused = true;
                a.pf_rotation =
                    cfg.playfield.rotation != "auto"
                        ? parse_rotation(cfg.playfield.rotation)
                        : auto_pf_rotation(displays[size_t(pf_by_name)], bg_by_name, displays);
                a.bg_rotation = parse_rotation(cfg.backglass.rotation);
                return a;
            }
        }
    }

    // --- 3. heuristic scoring ---
    std::vector<const DisplayInfo*> portrait;
    std::vector<const DisplayInfo*> landscape;
    for (const DisplayInfo& d : displays) {
        if (float(d.h) / float(d.w) >= 1.4f) {
            portrait.push_back(&d);
        } else if (float(d.w) / float(d.h) >= 1.4f) {
            landscape.push_back(&d);
        }
    }
    if (pf == -1) {
        const std::vector<const DisplayInfo*>* pool =
            !portrait.empty() ? &portrait : (!landscape.empty() ? &landscape : nullptr);
        if (pool == nullptr) {
            pool = &portrait; // both empty -> ds itself (below rebuilds)
        }
        std::vector<const DisplayInfo*> all;
        if (portrait.empty() && landscape.empty()) {
            for (const DisplayInfo& d : displays) {
                all.push_back(&d);
            }
            pool = &all;
        }
        const DisplayInfo* best = nullptr;
        for (const DisplayInfo* d : *pool) {
            if (best == nullptr || pf_beats(*d, *best)) {
                best = d;
            }
        }
        pf = best != nullptr ? best->index : -1;
    }
    if (cfg.backglass.match == "auto" && bg == -1 && cfg.backglass.enabled) {
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

    // --- 4. rotation resolution ---
    a.playfield = pf;
    a.backglass = bg;
    a.pf_rotation = cfg.playfield.rotation != "auto"
                        ? parse_rotation(cfg.playfield.rotation)
                        : (pf >= 0 ? auto_pf_rotation(displays[size_t(pf)], bg, displays) : 0);
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
