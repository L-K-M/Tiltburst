#include "table/validator.h"

#include "audio/audio_json.h"
#include "render/tbart.h"
#include "sim/math.h"
#include "sim/solver.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace tb::table {

using sim::Vec2;

using nlohmann::ordered_json;

namespace {

// ---- severity table (09 §8, fixed per code) ----
const char* severity_of(const std::string& code) {
    static const std::set<std::string> kWarnings = {
        "V006",
        "V007",
        "V012",
        "V020",
        "V025",
        "V026",
        "V027",
        "V031",
        "V032",
        "V033",
        "V035",
        "V036",
        "V038",
        "V039",
    };
    return kWarnings.count(code) != 0 ? "warning" : "error";
}

// 13-art-direction.md §6 function tags (V031 vocabulary).
const char* const kLightFunctions[] = {
    "shot_arrow",
    "jackpot",
    "lock",
    "bonus",
    "ball_save",
    "shoot_again",
    "extra_ball",
    "special",
    "mode",
    "objective",
    "progression",
    "multiball_ready",
    "status",
};

struct Diags {
    std::vector<ValidationDiag> out;

    void add(const std::string& file,
             const std::string& pointer,
             const std::string& code,
             const std::string& message) {
        out.push_back({file, pointer, code, severity_of(code), message});
    }
};

bool read_file(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// ---- geometry helpers (V004/V005; local 2-D on TableDef values) ----

struct Bounds {
    float lo[2] = {1e9f, 1e9f};
    float hi[2] = {-1e9f, -1e9f};

    void add(float x, float y, float pad = 0.0f) {
        lo[0] = std::min(lo[0], x - pad);
        lo[1] = std::min(lo[1], y - pad);
        hi[0] = std::max(hi[0], x + pad);
        hi[1] = std::max(hi[1], y + pad);
    }

    // 1 mm tolerance: geometry may touch the boundary exactly (the
    // plunger lane and orbit gates do by construction).
    bool outside(float w, float h) const {
        constexpr float kEps = 0.001f;
        return lo[0] < -kEps || lo[1] < -kEps || hi[0] > w + kEps || hi[1] > h + kEps;
    }
};

Vec2 node_point(const PathNode& n) {
    return {n.point[0], n.point[1]};
}

// ---- static-collider distance (occupancy grid, §8.1) ----

float dist_point_seg(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = {b.x - a.x, b.y - a.y};
    const float ab2 = ab.x * ab.x + ab.y * ab.y;
    float t = ab2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / ab2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 q = {a.x + ab.x * t, a.y + ab.y * t};
    return std::hypot(p.x - q.x, p.y - q.y);
}

// Distance from p to a collider's solid boundary (0 inside). Layer 0
// static solids only — the caller filters.
float dist_to_collider(Vec2 p, const sim::Collider& c) {
    switch (c.kind) {
    case sim::Collider::Kind::Point:
        return std::max(0.0f, std::hypot(p.x - c.a.x, p.y - c.a.y) - c.radius);
    case sim::Collider::Kind::Segment:
        return dist_point_seg(p, c.a, c.b);
    case sim::Collider::Kind::Arc: {
        const float d = std::hypot(p.x - c.a.x, p.y - c.a.y);
        // Inside the annulus band: on the wall.
        if (std::fabs(d - c.radius) < 0.001f) {
            return 0.0f;
        }
        // Angle test for the arc's wedge.
        float ang = std::atan2(p.y - c.a.y, p.x - c.a.x);
        if (c.a1 >= c.a0) {
            while (ang < c.a0) {
                ang += float(2.0 * M_PI);
            }
        } else {
            while (ang > c.a0) {
                ang -= float(2.0 * M_PI);
            }
        }
        const bool inside_wedge = c.a1 >= c.a0 ? (ang <= c.a1) : (ang >= c.a1);
        if (inside_wedge) {
            return std::fabs(d - c.radius);
        }
        // Nearest end cap chord endpoint.
        const Vec2 e0 = {c.a.x + c.radius * std::cos(c.a0), c.a.y + c.radius * std::sin(c.a0)};
        const Vec2 e1 = {c.a.x + c.radius * std::cos(c.a1), c.a.y + c.radius * std::sin(c.a1)};
        return std::min(
            dist_point_seg(p, e0, e1),
            std::min(std::hypot(p.x - e0.x, p.y - e0.y), std::hypot(p.x - e1.x, p.y - e1.y)));
    }
    }
    return 0.0f;
}

// ---- occupancy grid (§8.1) ----
struct Grid {
    float x0 = 0, y0 = 0;
    int nx = 0, ny = 0;
    float cell = 0.002f;
    std::vector<float> clearance; // per cell

    Vec2 center(int ix, int iy) const {
        return {x0 + (float(ix) + 0.5f) * cell, y0 + (float(iy) + 0.5f) * cell};
    }

    int index(int ix, int iy) const { return iy * nx + ix; }

    bool passable(int ix, int iy) const { return clearance[index(ix, iy)] >= 0.0135f; }
};

Grid build_grid(const sim::SimState& s, float w, float h) {
    constexpr float kPad = 0.028f; // one ball diameter, 14 cells
    Grid g;
    g.x0 = -kPad;
    g.y0 = -kPad;
    g.nx = int(std::ceil((w + 2 * kPad) / g.cell));
    g.ny = int(std::ceil((h + 2 * kPad) / g.cell));
    g.clearance.resize(size_t(g.nx) * size_t(g.ny), 1e9f);

    // Static layer-0 solids. Flippers count (sweep volume at rest);
    // gates/spinners never block (§8.1 step 2) — build_sim marks
    // flippers separately from colliders, so approximate each lower
    // flipper as its rest capsule (a segment, radius = base).
    struct Solid {
        int kind; // 0 seg, 1 point
        Vec2 a{}, b{};
        float r = 0;
    };

    std::vector<Solid> solids;
    for (const sim::Collider& c : s.colliders) {
        if (c.layer != 0) {
            continue;
        }
        if (c.kind == sim::Collider::Kind::Segment) {
            solids.push_back({0, c.a, c.b, 0.0f});
        } else if (c.kind == sim::Collider::Kind::Point) {
            solids.push_back({1, c.a, c.a, c.radius});
        } else {
            // Arc: approximate with a polyline of 16 chords (2 mm
            // grid resolution makes finer sampling pointless).
            const int steps = 16;
            for (int i = 0; i < steps; ++i) {
                const float a0 = c.a0 + (c.a1 - c.a0) * float(i) / float(steps);
                const float a1 = c.a0 + (c.a1 - c.a0) * float(i + 1) / float(steps);
                solids.push_back(
                    {0,
                     {c.a.x + c.radius * std::cos(a0), c.a.y + c.radius * std::sin(a0)},
                     {c.a.x + c.radius * std::cos(a1), c.a.y + c.radius * std::sin(a1)},
                     0.0f});
            }
        }
    }
    for (const sim::Flipper& f : s.flippers) {
        const float th = f.params.rest_rad();
        const Vec2 tip = {f.params.pivot.x + f.params.length * std::cos(th),
                          f.params.pivot.y + f.params.length * std::sin(th)};
        solids.push_back({0, f.params.pivot, tip, f.params.radius_base});
    }

    for (int iy = 0; iy < g.ny; ++iy) {
        for (int ix = 0; ix < g.nx; ++ix) {
            const Vec2 p = g.center(ix, iy);
            float best = 1e9f;
            for (const Solid& so : solids) {
                float d;
                if (so.kind == 1) {
                    d = std::max(0.0f, std::hypot(p.x - so.a.x, p.y - so.a.y) - so.r);
                } else if (so.r > 0.0f) {
                    d = std::max(0.0f, dist_point_seg(p, so.a, so.b) - so.r);
                } else {
                    d = dist_point_seg(p, so.a, so.b);
                }
                if (d < best) {
                    best = d;
                }
            }
            g.clearance[size_t(g.index(ix, iy))] = best;
        }
    }
    return g;
}

// 4-connected flood fill over passable cells; marks visited.
void flood(Grid& g, int sx, int sy, std::vector<uint8_t>& vis) {
    if (sx < 0 || sy < 0 || sx >= g.nx || sy >= g.ny) {
        return;
    }
    std::vector<std::pair<int, int>> stack;
    const auto push = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < g.nx && y < g.ny && !vis[size_t(g.index(x, y))] &&
            g.passable(x, y)) {
            vis[size_t(g.index(x, y))] = 1;
            stack.emplace_back(x, y);
        }
    };
    push(sx, sy);
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        push(x + 1, y);
        push(x - 1, y);
        push(x, y + 1);
        push(x, y - 1);
    }
}

// ---- metric-path grammar (09 §2; V029) ----
bool valid_metric_path(const std::string& path) {
    static const std::set<std::string> kTop = {
        "table",
        "runs",
        "skill",
        "seed",
        "shape",
        "balls",
        "script_errors",
        "stuck_balls",
        "flips_per_ball",
        "tilt_warnings_per_game",
        "tilts",
        "ball_saves_used_per_game",
    };
    static const std::set<std::string> kGroups = {
        "ball_time_s", "drains", "coverage", "score", "modes"};
    static const std::set<std::string> kLeaves = {
        "p10",
        "p50",
        "p90",
        "per_ball_p50",
        "center",
        "left_outlane",
        "right_outlane",
        "outlane_share",
        "hit",
        "total",
        "share",
        "started_per_game",
        "completed_per_game",
        "multiball_reach_share",
        "wizard_reach_share",
    };
    if (path.rfind("shots[", 0) == 0) {
        const size_t close = path.find(']');
        if (close == std::string::npos || close + 1 >= path.size() || path[close + 1] != '.') {
            return false;
        }
        const std::string field = path.substr(close + 2);
        return field == "attempts" || field == "made" || field == "rate";
    }
    std::vector<std::string> segs;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '.')) {
        segs.push_back(item);
    }
    if (segs.empty() || segs.size() > 3) {
        return false;
    }
    for (const std::string& s : segs) {
        if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) {
            return false;
        }
        for (char c : s) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return false;
            }
        }
    }
    // Must name an existing report key: top-level, group.leaf, or a
    // bare group name is NOT a leaf (report groups are objects).
    if (segs.size() == 1) {
        return kTop.count(segs[0]) != 0;
    }
    if (!kGroups.count(segs[0])) {
        return false;
    }
    return kLeaves.count(segs[1]) != 0;
}

// Linear relative luminance (13 §2.3).
float srgb_luminance(uint32_t rgb) {
    const auto chan = [](uint32_t v) {
        const float c = float(v) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * chan((rgb >> 16) & 0xFF) + 0.7152f * chan((rgb >> 8) & 0xFF) +
           0.0722f * chan(rgb & 0xFF);
}

} // namespace

std::string ValidationDiag::line() const {
    std::string l = file;
    if (!pointer.empty()) {
        l += ":" + pointer;
    }
    l += " [" + code + "][" + severity + "] " + message;
    return l;
}

std::string ValidationDiag::json() const {
    ordered_json j;
    j["file"] = file;
    j["pointer"] = pointer;
    j["code"] = code;
    j["severity"] = severity;
    j["message"] = message;
    return j.dump();
}

const char* diag_severity(const std::string& code) {
    return severity_of(code);
}

std::vector<ValidationDiag> validate_pack(const std::filesystem::path& dir) {
    Diags d;

    // ---- table.json via the in-game loader (V000–V031 schema checks) ----
    TableDef def;
    try {
        def = load_table(dir);
    } catch (const TableLoadError& e) {
        // The loader owns the schema checks; surface its first error.
        // Codes are embedded in its messages; default to V018.
        std::string code = "V018";
        const size_t v = std::string(e.what()).find("V0");
        if (v != std::string::npos && v + 3 < std::string(e.what()).size() + 1) {
            const std::string cand = std::string(e.what()).substr(v, 4);
            if (cand[1] == '0' && cand[2] >= '0' && cand[2] <= '9' && cand[3] >= '0' &&
                cand[3] <= '9') {
                code = cand;
            }
        }
        const std::string fname =
            e.file.empty() ? "table.json" : e.file.filename().generic_string();
        d.add(fname, e.json_pointer, code, e.what());
        return d.out; // fix the first error; geometry cascades (14 §8.1)
    } catch (const std::exception& e) {
        d.add("table.json", "", "V000", std::string("load failed: ") + e.what());
        return d.out;
    }

    // Raw JSON for the fields TableDef does not carry.
    std::string raw_text;
    ordered_json doc;
    if (read_file(dir / "table.json", raw_text)) {
        try {
            doc = ordered_json::parse(raw_text, nullptr, true, true);
        } catch (const std::exception&) {
            // The loader already passed; treat a re-parse failure as
            // unreachable.
        }
    }

    // ---- V025: slug equals the directory name ----
    {
        const std::string dirname = dir.filename().generic_string();
        if (!def.slug.empty() && def.slug != dirname) {
            d.add("table.json",
                  "/meta/slug",
                  "V025",
                  "slug '" + def.slug + "' != directory '" + dirname + "'");
        }
    }

    // ---- V004: element geometry inside [0,0]..size ----
    {
        const float w = def.width;
        const float h = def.height;
        const auto check = [&](const Element& e, const Bounds& b) {
            if (b.outside(w, h)) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "[%.3f, %.3f]", b.lo[0], b.lo[1]);
                d.add("table.json",
                      "",
                      "V004",
                      "'" + e.id() + "' extends outside the playfield at " + buf);
            }
        };
        for (const Element& e : def.elements) {
            Bounds b;
            if (std::holds_alternative<WallDef>(e.def)) {
                for (const PathNode& n : std::get<WallDef>(e.def).path) {
                    b.add(n.point[0], n.point[1]);
                }
            } else if (std::holds_alternative<PostDef>(e.def)) {
                const PostDef& p = std::get<PostDef>(e.def);
                b.add(p.pos[0], p.pos[1], p.radius);
            } else if (std::holds_alternative<FlipperDef>(e.def)) {
                // True swept bbox: base circle + tip arc across the
                // swing (a circular pad over-approximates and flags
                // flippers whose tip legitimately grazes a wall).
                const FlipperDef& f = std::get<FlipperDef>(e.def);
                b.add(f.pos[0], f.pos[1], f.radius_base);
                const float a0 = f.rest_angle_deg * float(M_PI) / 180.0f;
                const float a1 =
                    a0 + (f.left_side ? 1.0f : -1.0f) * f.swing_deg * float(M_PI) / 180.0f;
                for (int k = 0; k <= 16; ++k) {
                    const float th = a0 + (a1 - a0) * float(k) / 16.0f;
                    b.add(f.pos[0] + f.length * std::cos(th),
                          f.pos[1] + f.length * std::sin(th),
                          f.radius_tip);
                }
            } else if (std::holds_alternative<PlungerDef>(e.def)) {
                const PlungerDef& p = std::get<PlungerDef>(e.def);
                b.add(p.pos[0], p.pos[1]);
            } else if (std::holds_alternative<OutholeDef>(e.def)) {
                const OutholeDef& o = std::get<OutholeDef>(e.def);
                b.add(o.a[0], o.a[1]);
                b.add(o.b[0], o.b[1]);
            } else if (std::holds_alternative<LightDef>(e.def)) {
                const LightDef& l = std::get<LightDef>(e.def);
                b.add(l.pos[0], l.pos[1], l.size);
            } else if (std::holds_alternative<GateDef>(e.def)) {
                // The span is `width` END TO END (§5.5 gates span the
                // lane): half-width from center.
                const GateDef& g = std::get<GateDef>(e.def);
                b.add(g.pos[0], g.pos[1], g.width * 0.5f);
            } else if (std::holds_alternative<RolloverDef>(e.def)) {
                const RolloverDef& r = std::get<RolloverDef>(e.def);
                b.add(r.pos[0], r.pos[1], 0.010f);
            } else if (std::holds_alternative<SpinnerDef>(e.def)) {
                // TableDef carries no plate length; the trigger zone
                // is what the format can bound (approximation).
                const SpinnerDef& sp = std::get<SpinnerDef>(e.def);
                b.add(sp.pos[0], sp.pos[1], 0.005f);
            } else if (std::holds_alternative<RampDef>(e.def)) {
                for (const PathNode& n : std::get<RampDef>(e.def).path) {
                    b.add(n.point[0], n.point[1]);
                }
            } else if (std::holds_alternative<MagnetDef>(e.def)) {
                const MagnetDef& m = std::get<MagnetDef>(e.def);
                b.add(m.pos[0], m.pos[1]);
            } else if (std::holds_alternative<KickerDef>(e.def)) {
                const KickerDef& k = std::get<KickerDef>(e.def);
                b.add(k.pos[0], k.pos[1], k.radius);
            } else if (std::holds_alternative<DropTargetBankDef>(e.def)) {
                for (const auto& t : std::get<DropTargetBankDef>(e.def).targets) {
                    b.add(t.pos[0], t.pos[1], t.width);
                }
            } else if (std::holds_alternative<CaptiveBallDef>(e.def)) {
                const CaptiveBallDef& c = std::get<CaptiveBallDef>(e.def);
                b.add(c.a[0], c.a[1]);
                b.add(c.b[0], c.b[1]);
            } else if (std::holds_alternative<BallLockDef>(e.def)) {
                const BallLockDef& bl = std::get<BallLockDef>(e.def);
                b.add(bl.pos[0], bl.pos[1], 0.02f);
            } else if (std::holds_alternative<SlingshotDef>(e.def)) {
                const SlingshotDef& s = std::get<SlingshotDef>(e.def);
                b.add(s.face_a[0], s.face_a[1]);
                b.add(s.face_b[0], s.face_b[1]);
            } else if (std::holds_alternative<PopBumperDef>(e.def)) {
                const PopBumperDef& p = std::get<PopBumperDef>(e.def);
                b.add(p.pos[0], p.pos[1], p.radius);
            } else if (std::holds_alternative<StandupTargetDef>(e.def)) {
                const StandupTargetDef& st = std::get<StandupTargetDef>(e.def);
                b.add(st.pos[0], st.pos[1], st.width);
            }
            check(e, b);
        }
    }

    // ---- V005: lower flipper tip gap ----
    {
        const FlipperDef* left = nullptr;
        const FlipperDef* right = nullptr;
        for (const Element& e : def.elements) {
            if (!std::holds_alternative<FlipperDef>(e.def)) {
                continue;
            }
            const FlipperDef& f = std::get<FlipperDef>(e.def);
            if (f.input != "left" && f.input != "right") {
                continue;
            }
            const FlipperDef*& slot = f.input == "left" ? left : right;
            if (slot == nullptr || f.pos[1] < slot->pos[1]) {
                slot = &f;
            }
        }
        if (left != nullptr && right != nullptr) {
            const auto tip = [](const FlipperDef* f) {
                const float th = f->rest_angle_deg * float(M_PI) / 180.0f;
                return Vec2{f->pos[0] + f->length * std::cos(th),
                            f->pos[1] + f->length * std::sin(th)};
            };
            const Vec2 tl = tip(left);
            const Vec2 tr = tip(right);
            const float gap = std::hypot(tl.x - tr.x, tl.y - tr.y);
            if (gap < 0.054f || gap > 0.090f) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "flipper tip gap %.3f outside [0.054, 0.090]", gap);
                d.add("table.json", "", "V005", buf);
            } else if (gap < 0.062f || gap > 0.074f) {
                char buf[96];
                std::snprintf(buf,
                              sizeof(buf),
                              "flipper tip gap %.3f outside recommended [0.062, 0.074]",
                              gap);
                d.add("table.json", "", "V005", buf);
            }
        }
    }

    // ---- V015: one left and one right flipper ----
    {
        bool has_left = false;
        bool has_right = false;
        for (const Element& e : def.elements) {
            if (std::holds_alternative<FlipperDef>(e.def)) {
                const std::string& in = std::get<FlipperDef>(e.def).input;
                has_left = has_left || in == "left";
                has_right = has_right || in == "right";
            }
        }
        if (!has_left) {
            d.add("table.json", "", "V015", "no flipper bound to left");
        }
        if (!has_right) {
            d.add("table.json", "", "V015", "no flipper bound to right");
        }
    }

    // ---- V012: magnet sanity ----
    for (const Element& e : def.elements) {
        if (!std::holds_alternative<MagnetDef>(e.def)) {
            continue;
        }
        const MagnetDef& m = std::get<MagnetDef>(e.def);
        if (m.strength > 5.0f) {
            d.add("table.json",
                  "",
                  "V012",
                  "magnet '" + m.id + "' strength = " + std::to_string(m.strength) +
                      " is outside sane range");
        }
        if (m.radius < 0.03f || m.radius > 0.20f || m.radius <= 0.0135f) {
            d.add("table.json",
                  "",
                  "V012",
                  "magnet '" + m.id + "' radius = " + std::to_string(m.radius) +
                      " is outside sane range");
        }
    }

    // ---- V013: drop bank target faces do not overlap ----
    for (const Element& e : def.elements) {
        if (!std::holds_alternative<DropTargetBankDef>(e.def)) {
            continue;
        }
        const DropTargetBankDef& bank = std::get<DropTargetBankDef>(e.def);
        for (size_t i = 0; i + 1 < bank.targets.size(); ++i) {
            for (size_t j = i + 1; j < bank.targets.size(); ++j) {
                const auto& a = bank.targets[i];
                const auto& b = bank.targets[j];
                const float dist = std::hypot(a.pos[0] - b.pos[0], a.pos[1] - b.pos[1]);
                // 09 V013: centers >= width + 0.002 (one width — the
                // shared face width, not both summed).
                if (dist < std::max(a.width, b.width) + 0.002f) {
                    d.add("table.json",
                          "",
                          "V013",
                          "bank '" + bank.id + "' targets " + std::to_string(i) + "/" +
                              std::to_string(j) + " overlap");
                }
            }
        }
    }

    // ---- V016: performance caps ----
    {
        if (def.elements.size() > 400) {
            d.add("table.json",
                  "",
                  "V016",
                  std::to_string(def.elements.size()) + " expanded elements exceeds cap 400");
        }
        size_t nodes = 0;
        for (const Element& e : def.elements) {
            if (std::holds_alternative<WallDef>(e.def)) {
                nodes += std::get<WallDef>(e.def).path.size();
            } else if (std::holds_alternative<RampDef>(e.def)) {
                nodes += std::get<RampDef>(e.def).path.size();
            }
        }
        if (nodes > 2000) {
            d.add("table.json", "", "V016", std::to_string(nodes) + " path nodes exceeds cap 2000");
        }
    }

    // ---- V024: layer allowed per type (09 §4.21) ----
    {
        static const std::set<std::string> kLayer0Only = {
            "plunger", "drop_target_bank", "ball_lock", "outhole", "trough", "ramp"};
        for (const Element& e : def.elements) {
            const int layer = [&] {
                if (std::holds_alternative<WallDef>(e.def)) {
                    return std::get<WallDef>(e.def).layer;
                }
                if (std::holds_alternative<PostDef>(e.def)) {
                    return std::get<PostDef>(e.def).layer;
                }
                if (std::holds_alternative<FlipperDef>(e.def)) {
                    return std::get<FlipperDef>(e.def).layer;
                }
                if (std::holds_alternative<LightDef>(e.def)) {
                    return std::get<LightDef>(e.def).layer;
                }
                if (std::holds_alternative<GateDef>(e.def)) {
                    return std::get<GateDef>(e.def).layer;
                }
                if (std::holds_alternative<SpinnerDef>(e.def)) {
                    return std::get<SpinnerDef>(e.def).layer;
                }
                if (std::holds_alternative<RampDef>(e.def)) {
                    return std::get<RampDef>(e.def).layer;
                }
                if (std::holds_alternative<MagnetDef>(e.def)) {
                    return std::get<MagnetDef>(e.def).layer;
                }
                if (std::holds_alternative<KickerDef>(e.def)) {
                    return std::get<KickerDef>(e.def).layer;
                }
                if (std::holds_alternative<DropTargetBankDef>(e.def)) {
                    return std::get<DropTargetBankDef>(e.def).layer;
                }
                if (std::holds_alternative<CaptiveBallDef>(e.def)) {
                    return std::get<CaptiveBallDef>(e.def).layer;
                }
                return 0;
            }();
            if (layer != 0 && kLayer0Only.count(e.type_name()) != 0) {
                d.add("table.json",
                      "",
                      "V024",
                      "'" + e.id() + "' (" + e.type_name() + ") not allowed on layer " +
                          std::to_string(layer));
            }
        }
    }

    // ---- V027 / V031: light color and function (raw JSON) ----
    if (doc.is_object()) {
        if (doc.contains("elements") && doc.at("elements").is_array()) {
            size_t idx = 0;
            for (const auto& el : doc.at("elements")) {
                if (el.value("type", std::string()) == "light") {
                    const std::string lid = el.value("id", std::string("?"));
                    if (el.contains("color")) {
                        const std::string c = el.at("color").get<std::string>();
                        const bool hex = c.size() == 7 && c[0] == '#';
                        // Palette roles resolve via art.json below;
                        // hex needs only shape here.
                        if (!hex) {
                            // Unknown role until art.json parses (V027
                            // is warning-grade; art load reports V034).
                            const char* roles[] = {"primary",
                                                   "secondary",
                                                   "accent1",
                                                   "accent2",
                                                   "warm",
                                                   "glow_white",
                                                   "bg0",
                                                   "bg1"};
                            bool known = false;
                            for (const char* r : roles) {
                                known = known || c == r;
                            }
                            if (!known) {
                                d.add("table.json",
                                      "/elements/" + std::to_string(idx),
                                      "V027",
                                      "light '" + lid + "' color '" + c +
                                          "' unresolvable; will render white");
                            }
                        }
                    }
                    if (el.contains("function")) {
                        const std::string f = el.at("function").get<std::string>();
                        bool known = false;
                        for (const char* fn : kLightFunctions) {
                            known = known || f == fn;
                        }
                        if (!known) {
                            d.add("table.json",
                                  "/elements/" + std::to_string(idx),
                                  "V031",
                                  "light '" + lid + "' function '" + f + "' unknown");
                        }
                    }
                }
                ++idx;
            }
        }

        // ---- V029: autoplay_bounds ----
        if (doc.contains("meta") && doc.at("meta").contains("autoplay_bounds")) {
            const auto& ab = doc.at("meta").at("autoplay_bounds");
            if (ab.is_object()) {
                for (auto it = ab.begin(); it != ab.end(); ++it) {
                    const std::string detail = [&] {
                        if (!valid_metric_path(it.key())) {
                            return std::string("not a report metric path");
                        }
                        if (!it->is_object() || (!it->contains("min") && !it->contains("max"))) {
                            return std::string("at least one of min/max required");
                        }
                        if (it->contains("min") && it->contains("max")) {
                            if (!it->at("min").is_number() || !it->at("max").is_number()) {
                                return std::string("min/max must be numeric");
                            }
                            if (it->at("min").get<double>() > it->at("max").get<double>()) {
                                return std::string("min > max");
                            }
                        }
                        if (!it->contains("skill") || !it->at("skill").is_number() ||
                            it->at("skill").get<int>() < 0 || it->at("skill").get<int>() > 2) {
                            return std::string("skill must be 0, 1, or 2");
                        }
                        return std::string();
                    }();
                    if (!detail.empty()) {
                        d.add("table.json",
                              "/meta/autoplay_bounds/" + it.key(),
                              "V029",
                              "autoplay_bounds '" + it.key() + "': " + detail);
                    }
                }
            }
        }

        // ---- V030: light_groups ----
        if (doc.contains("light_groups")) {
            const auto& lgs = doc.at("light_groups");
            if (lgs.is_object()) {
                std::set<std::string> element_ids;
                for (const Element& e : def.elements) {
                    element_ids.insert(e.id());
                }
                for (auto it = lgs.begin(); it != lgs.end(); ++it) {
                    const std::string gid = it.key();
                    if (element_ids.count(gid) != 0) {
                        d.add("table.json",
                              "/light_groups/" + gid,
                              "V030",
                              "light group '" + gid + "': id collides with an element");
                        continue;
                    }
                    if (!it->is_array()) {
                        d.add("table.json",
                              "/light_groups/" + gid,
                              "V030",
                              "light group '" + gid + "': members must be an array");
                        continue;
                    }
                    std::set<std::string> seen;
                    for (const auto& m : *it) {
                        if (!m.is_string()) {
                            d.add("table.json",
                                  "/light_groups/" + gid,
                                  "V030",
                                  "light group '" + gid + "': member must be a string");
                            continue;
                        }
                        const std::string mid = m.get<std::string>();
                        if (seen.count(mid) != 0) {
                            d.add("table.json",
                                  "/light_groups/" + gid,
                                  "V030",
                                  "light group '" + gid + "': member '" + mid + "' appears twice");
                        }
                        seen.insert(mid);
                        // Member must exist and be a light.
                        bool found = false;
                        for (const Element& e : def.elements) {
                            if (e.id() == mid) {
                                found = std::holds_alternative<LightDef>(e.def);
                                break;
                            }
                        }
                        if (!found) {
                            d.add("table.json",
                                  "/light_groups/" + gid,
                                  "V030",
                                  "light group '" + gid + "': member '" + mid +
                                      "' is not a light element");
                        }
                    }
                }
            }
        }
    }

    // ---- spatial checks on the occupancy grid (V003-full, V006, V007) ----
    {
        sim::SimState s;
        build_sim(def, s);
        Grid g = build_grid(s, def.width, def.height);

        // V003 (watertight): flood from the padded corner cell — a
        // cell outside the boundary by construction (§8.1 step 1) —
        // and from the plunger (inside). Watertight means the two
        // fills never meet. (The rounded corners' notches are outside
        // the boundary but inside the rect, so a rect test would leak
        // every table.)
        std::vector<uint8_t> exterior(size_t(g.nx) * size_t(g.ny), 0);
        flood(g, 0, 0, exterior);
        // The interior of the boundary: start from the plunger cell.
        Vec2 plunger_pos{def.width * 0.5f, 0.03f};
        for (const Element& e : def.elements) {
            if (std::holds_alternative<PlungerDef>(e.def)) {
                const PlungerDef& p = std::get<PlungerDef>(e.def);
                plunger_pos = {p.pos[0], p.pos[1]};
                break;
            }
        }
        int pix = int((plunger_pos.x - g.x0) / g.cell);
        int piy = int((plunger_pos.y - g.y0) / g.cell);
        // The plunger pos cell can sit INSIDE the lane's bottom cap
        // (the resting ball hugs it, so clearance < ball radius there).
        // §8.1's fill is "from the plunger" — seed the nearest passable
        // cell, preferring up-lane (+y) where the ball launches.
        if (!g.passable(pix, piy)) {
            bool seeded = false;
            for (int dy = 0; dy <= 25 && !seeded; ++dy) {
                for (int dx = -dy; dx <= dy && !seeded; ++dx) {
                    const int cx = pix + dx;
                    const int cy = piy + dy;
                    if (cx >= 0 && cy >= 0 && cx < g.nx && cy < g.ny && g.passable(cx, cy)) {
                        pix = cx;
                        piy = cy;
                        seeded = true;
                    }
                }
            }
        }
        std::vector<uint8_t> interior(size_t(g.nx) * size_t(g.ny), 0);
        flood(g, pix, piy, interior);
        bool leak_reported = false;
        for (int iy = 0; iy < g.ny && !leak_reported; ++iy) {
            for (int ix = 0; ix < g.nx && !leak_reported; ++ix) {
                if (exterior[size_t(g.index(ix, iy))] && interior[size_t(g.index(ix, iy))]) {
                    const Vec2 c = g.center(ix, iy);
                    char buf[80];
                    std::snprintf(buf, sizeof(buf), "[%.3f, %.3f]", c.x, c.y);
                    d.add(
                        "table.json", "", "V003", std::string("outer boundary leaks near ") + buf);
                    leak_reported = true;
                }
            }
        }

        // ---- V006: tight corridors, sampled at clearance RIDGE cells
        // only (§8.1: evaluating every passable cell fires beside every
        // wall — the 3 mm band under 0.0165 clearance exists along all
        // of them). A ridge cell: a local clearance maximum along at
        // least one of the 4 sampling axes. Chains > 0.020 m of tight
        // ridge cells that the plunger fill reaches report once.
        {
            std::vector<uint8_t>& reached = interior;
            std::vector<uint8_t> ridge(size_t(g.nx) * size_t(g.ny), 0);
            for (int iy = 1; iy + 1 < g.ny; ++iy) {
                for (int ix = 1; ix + 1 < g.nx; ++ix) {
                    const size_t idx = size_t(g.index(ix, iy));
                    if (!g.passable(ix, iy)) {
                        continue;
                    }
                    const float c = g.clearance[idx];
                    const float e[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
                    for (const auto& [dx, dy] : e) {
                        const float ca = g.clearance[size_t(g.index(ix + dx, iy + dy))];
                        const float cb = g.clearance[size_t(g.index(ix - dx, iy - dy))];
                        if (ca <= c && cb <= c && (ca < c || cb < c)) {
                            ridge[idx] = 1;
                            break;
                        }
                    }
                }
            }
            // 8-connected chains of tight ridge cells.
            std::vector<uint8_t> reported6(size_t(g.nx) * size_t(g.ny), 0);
            for (int iy = 0; iy < g.ny; ++iy) {
                for (int ix = 0; ix < g.nx; ++ix) {
                    const size_t idx = size_t(g.index(ix, iy));
                    if (reported6[idx] || !ridge[idx] || !reached[idx]) {
                        continue;
                    }
                    if (g.clearance[idx] >= 0.0165f) {
                        continue; // not tight; chains seed from tight cells
                    }
                    // BFS the tight-ridge chain (8-connected).
                    std::vector<std::pair<int, int>> chain{{ix, iy}};
                    std::vector<std::pair<int, int>> stack{{ix, iy}};
                    reported6[idx] = 1;
                    float narrowest = g.clearance[idx];
                    std::pair<int, int> narrow_cell{ix, iy};
                    while (!stack.empty()) {
                        auto [cx, cy] = stack.back();
                        stack.pop_back();
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) {
                                    continue;
                                }
                                const int qx = cx + dx;
                                const int qy = cy + dy;
                                if (qx < 0 || qy < 0 || qx >= g.nx || qy >= g.ny) {
                                    continue;
                                }
                                const size_t q = size_t(g.index(qx, qy));
                                if (reported6[q] || !ridge[q] || !reached[q] ||
                                    g.clearance[q] >= 0.0165f) {
                                    continue;
                                }
                                reported6[q] = 1;
                                chain.emplace_back(qx, qy);
                                stack.emplace_back(qx, qy);
                                if (g.clearance[q] < narrowest) {
                                    narrowest = g.clearance[q];
                                    narrow_cell = {qx, qy};
                                }
                            }
                        }
                    }
                    const float length_m = float(chain.size()) * g.cell; // ~chain extent
                    if (length_m > 0.020f) {
                        const Vec2 c = g.center(narrow_cell.first, narrow_cell.second);
                        char buf[96];
                        std::snprintf(buf,
                                      sizeof(buf),
                                      "lane near [%.3f, %.3f] is %.3f wide (< 0.033)",
                                      c.x,
                                      c.y,
                                      2.0f * narrowest);
                        d.add("table.json", "", "V006", buf);
                    }
                }
            }
        }

        // ---- V014: gate orientation — a 0.030 ray from pos along
        // facing must not hit a static solid; the span endpoints must
        // each sit within 0.006 of one (the gate closes a lane). The
        // plunger_lane prefab's sensor gates measure 0 by convention
        // (§5.2); skip `state_open` sensor gates the same way.
        for (const Element& e : def.elements) {
            if (!std::holds_alternative<GateDef>(e.def)) {
                continue;
            }
            const GateDef& gt = std::get<GateDef>(e.def);
            if (gt.state_open) {
                continue; // open sensor gate (§5.5): spans by convention
            }
            const float fr = gt.facing_deg * float(M_PI) / 180.0f;
            // Ray sample points along the facing.
            bool ray_blocked = false;
            for (int st = 1; st <= 6 && !ray_blocked; ++st) {
                const float t = 0.005f * float(st);
                const Vec2 p = {gt.pos[0] + t * std::cos(fr), gt.pos[1] + t * std::sin(fr)};
                for (const sim::Collider& c : s.colliders) {
                    if (c.layer != 0) {
                        continue;
                    }
                    if (dist_to_collider(p, c) <= 0.0f) {
                        ray_blocked = true;
                        break;
                    }
                }
            }
            // Span endpoints: pos +/- (width/2) perpendicular to facing.
            const float pr = fr + float(M_PI) / 2.0f;
            const Vec2 ends[2] = {{gt.pos[0] + gt.width * 0.5f * std::cos(pr),
                                   gt.pos[1] + gt.width * 0.5f * std::sin(pr)},
                                  {gt.pos[0] - gt.width * 0.5f * std::cos(pr),
                                   gt.pos[1] - gt.width * 0.5f * std::sin(pr)}};
            for (const Vec2& end : ends) {
                float best = 1e9f;
                for (const sim::Collider& c : s.colliders) {
                    if (c.layer != 0) {
                        continue;
                    }
                    best = std::min(best, dist_to_collider(end, c));
                }
                if (best > 0.006f) {
                    char buf[96];
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "span endpoint [%.3f, %.3f] anchors %.3f from a "
                                  "collider (max 0.006)",
                                  end.x,
                                  end.y,
                                  best);
                    d.add("table.json", "", "V014", "gate '" + gt.id + "' " + buf);
                }
            }
            if (ray_blocked) {
                d.add("table.json",
                      "",
                      "V014",
                      "gate '" + gt.id + "' faces into a wall along facing_deg " +
                          std::to_string(int(gt.facing_deg)));
            }
        }

        if (!leak_reported) {
            // V007: the plunger fill above IS the reached set; report
            // unreached passable regions > 250 cells (excluding the
            // exterior ring).
            std::vector<uint8_t>& reached = interior;
            std::vector<uint8_t> reported(size_t(g.nx) * size_t(g.ny), 0);
            for (int iy = 0; iy < g.ny; ++iy) {
                for (int ix = 0; ix < g.nx; ++ix) {
                    const size_t idx = size_t(g.index(ix, iy));
                    if (reached[idx] || reported[idx] || !g.passable(ix, iy) || exterior[idx]) {
                        continue;
                    }
                    // Measure this unreached component (4-connected).
                    std::vector<std::pair<int, int>> comp;
                    std::vector<std::pair<int, int>> stack{{ix, iy}};
                    reported[idx] = 1;
                    while (!stack.empty()) {
                        auto [cx, cy] = stack.back();
                        stack.pop_back();
                        comp.emplace_back(cx, cy);
                        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& [dx, dy] : nb) {
                            const int qx = cx + dx;
                            const int qy = cy + dy;
                            if (qx < 0 || qy < 0 || qx >= g.nx || qy >= g.ny) {
                                continue;
                            }
                            const size_t q = size_t(g.index(qx, qy));
                            if (!reached[q] && !reported[q] && g.passable(qx, qy) && !exterior[q]) {
                                reported[q] = 1;
                                stack.emplace_back(qx, qy);
                            }
                        }
                    }
                    bool touches_flipper_sweep = false;
                    for (const auto& [cx, cy] : comp) {
                        const Vec2 c = g.center(cx, cy);
                        for (const sim::Flipper& f : s.flippers) {
                            const float dist =
                                std::hypot(c.x - f.params.pivot.x, c.y - f.params.pivot.y);
                            if (dist <= f.params.length + f.params.radius_base + 0.0135f) {
                                touches_flipper_sweep = true;
                            }
                        }
                    }
                    if (comp.size() > 250 && !touches_flipper_sweep) {
                        const Vec2 c =
                            g.center(comp[comp.size() / 2].first, comp[comp.size() / 2].second);
                        char buf[96];
                        std::snprintf(buf,
                                      sizeof(buf),
                                      "region of %.3f m2 near [%.3f, %.3f] is unreachable "
                                      "from the plunger",
                                      float(comp.size()) * g.cell * g.cell,
                                      c.x,
                                      c.y);
                        d.add("table.json", "", "V007", buf);
                    }
                }
            }
            // The flipper area must be reachable: the cell between
            // the two lowest flippers' TIPS (the pivot cells sit inside
            // the bats themselves).
            bool flipper_area_reached = false;
            {
                const sim::Flipper* lo[2] = {nullptr, nullptr};
                for (const sim::Flipper& f : s.flippers) {
                    const int slot = f.params.side_sign > 0 ? 0 : 1;
                    if (lo[slot] == nullptr || f.params.pivot.y < lo[slot]->params.pivot.y) {
                        lo[slot] = &f;
                    }
                }
                if (lo[0] != nullptr && lo[1] != nullptr) {
                    const auto tip = [&](const sim::Flipper* f) {
                        const float th = f->params.rest_rad();
                        return Vec2{f->params.pivot.x + f->params.length * std::cos(th),
                                    f->params.pivot.y + f->params.length * std::sin(th)};
                    };
                    const Vec2 mid = {(tip(lo[0]).x + tip(lo[1]).x) * 0.5f,
                                      (tip(lo[0]).y + tip(lo[1]).y) * 0.5f};
                    // Scan a small neighborhood: the exact midpoint
                    // may sit on the drain edge.
                    for (int dy = -3; dy <= 3 && !flipper_area_reached; ++dy) {
                        for (int dx = -3; dx <= 3 && !flipper_area_reached; ++dx) {
                            const int fx = int((mid.x - g.x0) / g.cell) + dx;
                            const int fy = int((mid.y - g.y0) / g.cell) + dy;
                            if (fx >= 0 && fy >= 0 && fx < g.nx && fy < g.ny &&
                                reached[size_t(g.index(fx, fy))]) {
                                flipper_area_reached = true;
                            }
                        }
                    }
                }
            }
            if (!flipper_area_reached) {
                d.add("table.json", "", "V007", "the flipper area is unreachable from the plunger");
            }
        }
    }

    // ---- rules.lua lints (V002 warning side, V032, V033) ----
    std::string rules_src;
    const bool have_rules = read_file(dir / "rules.lua", rules_src);
    if (have_rules) {
        // V032: scoring elements never scored; V033: lights never driven.
        const auto mentioned = [&](const std::string& id) {
            return rules_src.find("\"" + id + "\"") != std::string::npos ||
                   rules_src.find("'" + id + "'") != std::string::npos;
        };
        for (const Element& e : def.elements) {
            // Everything that can emit a scoring event. Gates are
            // lane hardware (sensors/one-ways), not scoring elements;
            // lights and walls are excluded per 14 §8.2.
            const bool scoring = std::holds_alternative<RolloverDef>(e.def) ||
                                 std::holds_alternative<SpinnerDef>(e.def) ||
                                 std::holds_alternative<KickerDef>(e.def) ||
                                 std::holds_alternative<DropTargetBankDef>(e.def) ||
                                 std::holds_alternative<PopBumperDef>(e.def) ||
                                 std::holds_alternative<StandupTargetDef>(e.def) ||
                                 std::holds_alternative<SlingshotDef>(e.def);
            if (scoring && !mentioned(e.id())) {
                d.add("rules.lua",
                      "",
                      "V032",
                      "'" + e.id() + "' (" + e.type_name() + ") is never scored by rules.lua");
            }
            if (std::holds_alternative<LightDef>(e.def) && !mentioned(e.id())) {
                d.add(
                    "rules.lua", "", "V033", "light '" + e.id() + "' is never driven by rules.lua");
            }
        }
    }

    // ---- art.json (V034, V035, V036) ----
    std::string art_text;
    if (read_file(dir / "art.json", art_text)) {
        std::vector<std::pair<std::string, int>> light_ids;
        {
            int i = 0;
            for (const Element& e : def.elements) {
                if (std::holds_alternative<LightDef>(e.def)) {
                    light_ids.emplace_back(e.id(), i);
                }
                ++i;
            }
        }
        try {
            const render::TbArt& art = render::load_art(dir, light_ids).art;
            // V035: glow budget — emissive area fraction of the
            // playfield in the default state. Unlit inserts render at
            // the 15 % fill floor; count additive/glowing prims by
            // their bounding area (2-D approximation).
            float emissive = 0.0f;
            for (const auto& layer : art.layers) {
                for (const auto& prim : layer.prims) {
                    const bool additive = layer.additive;
                    if (!additive) {
                        continue;
                    }
                    float w = 0.0f;
                    float h = 0.0f;
                    switch (prim.kind) {
                    case render::ArtPrim::Kind::Rect:
                        w = prim.w;
                        h = prim.h;
                        break;
                    case render::ArtPrim::Kind::Circle:
                        w = h = prim.r * 2;
                        break;
                    case render::ArtPrim::Kind::Polyline:
                    case render::ArtPrim::Kind::Polygon: {
                        for (size_t pi = 0; pi + 1 < prim.points.size(); pi += 2) {
                            w = std::max(w, prim.points[pi]);
                            h = std::max(h, prim.points[pi + 1]);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                    emissive += w * h;
                }
            }
            const float area = def.width * def.height;
            const float pct = 100.0f * emissive / area;
            if (pct > 15.0f) {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "%.1f", pct);
                d.add("art.json",
                      "",
                      "V035",
                      std::string("glow budget ") + buf + " % of playfield exceeds 15 %");
            }
        } catch (const std::exception& e) {
            d.add("art.json", "", "V034", e.what());
        }
    }

    // ---- audio.json (V037, V038, V039) ----
    audio::TableAudio ta;
    bool have_audio = false;
    try {
        have_audio = audio::load_audio_json(dir, ta);
    } catch (const audio::AudioLoadError& e) {
        d.add("audio.json", e.json_pointer, "V037", e.what());
    }
    if (have_audio) {
        int purpose[int(sim::SimState::kSoundPurposeCount)] = {};
        try {
            auto bank = audio::build_bank(ta, dir, purpose);
            // V038: scoring events with no mapped sound — a purpose
            // explicitly disabled via "none" while the table's rules
            // reference its elements is the practical case; warn for
            // every disabled purpose.
            for (int i = 0; i < int(sim::SimState::kSoundPurposeCount); ++i) {
                if (purpose[i] < 0) {
                    d.add("audio.json",
                          "",
                          "V038",
                          std::string("scoring event '") +
                              audio::purpose_key(sim::SoundPurpose(i)) + "' has no mapped sound");
                }
            }
        } catch (const audio::AudioLoadError& e) {
            d.add("audio.json", e.json_pointer, "V037", e.what());
        }
        // V039: music states used by rules.lua without a song.
        if (have_rules) {
            static const char* const kStates[] = {
                "attract", "main", "mode", "multiball", "wizard", "game_over"};
            for (const char* st : kStates) {
                const std::string needle = std::string("\"") + st + "\"";
                if (rules_src.find("play_music") != std::string::npos &&
                    rules_src.find(needle) != std::string::npos) {
                    bool has = false;
                    for (const auto& [id, raw] : ta.songs) {
                        has = has || id == st;
                    }
                    if (!has) {
                        d.add("audio.json",
                              "",
                              "V039",
                              std::string("music state '") + st + "' has no song assigned");
                    }
                }
            }
        }
    }

    return d.out;
}

} // namespace tb::table
