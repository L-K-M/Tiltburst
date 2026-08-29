#include "render/tbart.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace tb::render {

using json = nlohmann::ordered_json; // §5.5: key order matters for ids

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& pointer) {
    throw ArtError("art.json: " + what, pointer);
}

constexpr float kPi = 3.14159265358979323846f;

uint8_t hex_nibble(char c, const std::string& pointer) {
    if (c >= '0' && c <= '9') {
        return uint8_t(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return uint8_t(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return uint8_t(c - 'A' + 10);
    }
    fail("bad hex digit '" + std::string(1, c) + "'", pointer);
}

uint32_t parse_hex_color(const std::string& hex, const std::string& pointer) {
    // "#RRGGBB" or "#RRGGBBAA".
    if (hex.size() != 7 && hex.size() != 9) {
        fail("hex color must be #RRGGBB or #RRGGBBAA: '" + hex + "'", pointer);
    }
    const uint8_t r = (hex_nibble(hex[1], pointer) << 4) | hex_nibble(hex[2], pointer);
    const uint8_t g = (hex_nibble(hex[3], pointer) << 4) | hex_nibble(hex[4], pointer);
    const uint8_t b = (hex_nibble(hex[5], pointer) << 4) | hex_nibble(hex[6], pointer);
    const uint8_t a =
        hex.size() == 9 ? uint8_t((hex_nibble(hex[7], pointer) << 4) | hex_nibble(hex[8], pointer))
                        : 0xFF;
    return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | uint32_t(a);
}

uint32_t role_color(const Palette& palette, int role) {
    return palette.rgba[size_t(role)];
}

} // namespace

// §2.2 canon palettes.
const Palette* canon_palette(const std::string& name) {
    static const std::vector<Palette> kPalettes = [] {
        std::vector<Palette> out;
        auto add = [&](const char* n, std::array<uint32_t, 8> rgba) {
            Palette p;
            p.name = n;
            p.rgba = rgba;
            out.push_back(p);
        };
        add("sunset-synth",
            {0x0D0221FF,
             0x26104DFF,
             0xFF2975FF,
             0x08F7FEFF,
             0x9D4EFFFF,
             0xFFD319FF,
             0xFF901FFF,
             0xFFF2E5FF});
        add("atomic-teal",
            {0x06181DFF,
             0x0C2E36FF,
             0x1BE7D2FF,
             0xF45D48FF,
             0xB4E33DFF,
             0xFFD166FF,
             0xFF9F1CFF,
             0xEAFFF8FF});
        add("arcade-purple",
            {0x120521FF,
             0x241040FF,
             0xB14EFFFF,
             0x00FFC6FF,
             0xFF3864FF,
             0x3D9BFFFF,
             0xFF8E3CFF,
             0xF4EDFFFF});
        add("vapor-pink",
            {0x1A0B2EFF,
             0x2E1650FF,
             0xFF71CEFF,
             0x01CDFEFF,
             0x05FFA1FF,
             0xB967FFFF,
             0xFFB347FF,
             0xFFF0FAFF});
        add("midnight-chrome",
            {0x05070EFF,
             0x131B2AFF,
             0x4DA6FFFF,
             0xC4CEDFFF,
             0x00F0FFFF,
             0xFF4D6DFF,
             0xFFA940FF,
             0xEFF5FFFF});
        return out;
    }();
    for (const Palette& p : kPalettes) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

int palette_role(const std::string& role) {
    static const char* const kRoles[8] = {
        "bg0", "bg1", "primary", "secondary", "accent1", "accent2", "warm", "glow_white"};
    for (int i = 0; i < 8; ++i) {
        if (role == kRoles[i]) {
            return i;
        }
    }
    return -1;
}

uint32_t resolve_color(const std::string& color, const Palette& palette) {
    if (!color.empty() && color[0] == '#') {
        return parse_hex_color(color, "");
    }
    const int role = palette_role(color);
    if (role < 0) {
        fail("unknown color '" + color + "'", "");
    }
    return role_color(palette, role);
}

namespace {

// ---- primitive parsing ----

Transform parse_transform(const json& t, const std::string& pointer) {
    Transform out;
    if (!t.is_object()) {
        fail("transform must be an object", pointer);
    }
    if (auto it = t.find("pos"); it != t.end() && it->is_array() && it->size() == 2) {
        out.pos[0] = float((*it)[0].get<double>());
        out.pos[1] = float((*it)[1].get<double>());
    } else {
        fail("transform.pos must be [x, y]", pointer + "/pos");
    }
    if (auto it = t.find("rot_deg"); it != t.end()) {
        out.rot_deg = float(it->get<double>());
    }
    if (auto it = t.find("scale"); it != t.end()) {
        const double s = it->get<double>();
        if (!(s > 0.0)) {
            fail("transform.scale must be > 0", pointer + "/scale");
        }
        out.scale = float(s);
    }
    return out;
}

Fill parse_fill(const json& f, const Palette& palette, const std::string& pointer) {
    Fill out;
    if (f.is_string()) {
        out.color0 = resolve_color(f.get<std::string>(), palette);
        return out;
    }
    if (!f.is_object()) {
        fail("fill must be a color string or a gradient object", pointer);
    }
    const std::string type = f.value("type", std::string());
    if (type != "linear" && type != "radial") {
        fail("fill.type must be linear|radial", pointer + "/type");
    }
    out.kind = type == "linear" ? Fill::Kind::Linear : Fill::Kind::Radial;
    if (auto it = f.find("colors"); it != f.end() && it->is_array() && it->size() == 2) {
        out.color0 = resolve_color((*it)[0].get<std::string>(), palette);
        out.color1 = resolve_color((*it)[1].get<std::string>(), palette);
    } else {
        fail("gradient colors must have exactly 2 stops", pointer + "/colors");
    }
    if (out.kind == Fill::Kind::Linear) {
        if (auto it = f.find("angle_deg"); it != f.end()) {
            out.angle_deg = float(it->get<double>());
        }
    }
    if (auto it = f.find("length"); it != f.end() && out.kind == Fill::Kind::Linear) {
        out.length = float(it->get<double>());
    }
    if (auto it = f.find("radius"); it != f.end() && out.kind == Fill::Kind::Radial) {
        out.length = float(it->get<double>());
    }
    if (out.kind == Fill::Kind::Radial && !(out.length > 0.0f)) {
        fail("radial fill radius must be > 0", pointer + "/radius");
    }
    return out;
}

Glow parse_glow(const json& g, const Palette& palette, const std::string& pointer) {
    Glow out;
    if (!g.is_object()) {
        fail("glow must be an object", pointer);
    }
    if (auto it = g.find("radius"); it != g.end()) {
        const double r = it->get<double>();
        if (!(r > 0.0)) {
            fail("glow.radius must be > 0", pointer + "/radius");
        }
        out.radius = float(r);
    }
    if (auto it = g.find("intensity"); it != g.end()) {
        const double v = it->get<double>();
        if (!(v >= 0.0 && v <= 2.0)) {
            fail("glow.intensity must be in [0, 2]", pointer + "/intensity");
        }
        out.intensity = float(v);
    }
    if (auto it = g.find("color"); it != g.end() && it->is_string()) {
        out.has_color = true;
        out.color = resolve_color(it->get<std::string>(), palette);
    }
    return out;
}

uint8_t font_id(const std::string& f, const std::string& pointer) {
    if (f == "orbitron") {
        return 0; // the HUD role face (ChakraPetch substitution ADR)
    }
    if (f == "monoton") {
        return 1;
    }
    if (f == "righteous") {
        return 2;
    }
    fail("unknown font '" + f + "'", pointer);
}

ArtPrim parse_prim(const json& p,
                   const Palette& palette,
                   const std::unordered_map<std::string, int>& light_ids);

// ---- decal prefabs (§4): expand into concrete children ----

// Applies the instance transform on top of the prefab's local geometry.
ArtPrim child_prim(const Transform& base, ArtPrim local) {
    // Compose: child's world = base * local (rot/scale then translate).
    const float rad = base.rot_deg * kPi / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float s = base.scale;
    auto map_point = [&](float& x, float& y) {
        const float rx = (x * cs - y * sn) * s;
        const float ry = (x * sn + y * cs) * s;
        x = rx + base.pos[0];
        y = ry + base.pos[1];
    };
    switch (local.kind) {
    case ArtPrim::Kind::Circle:
    case ArtPrim::Kind::Ring:
    case ArtPrim::Kind::Arc:
        map_point(local.transform.pos[0], local.transform.pos[1]);
        local.r *= s;
        local.thickness *= s;
        break;
    case ArtPrim::Kind::Rect:
        map_point(local.transform.pos[0], local.transform.pos[1]);
        local.w *= s;
        local.h *= s;
        local.corner_r *= s;
        break;
    case ArtPrim::Kind::Capsule:
    case ArtPrim::Kind::Segment: {
        map_point(local.a[0], local.a[1]);
        map_point(local.b[0], local.b[1]);
        local.r *= s;
        break;
    }
    case ArtPrim::Kind::Polyline:
    case ArtPrim::Kind::Polygon:
        for (size_t i = 0; i < local.points.size(); i += 2) {
            map_point(local.points[i], local.points[i + 1]);
        }
        break;
    case ArtPrim::Kind::Text:
        map_point(local.transform.pos[0], local.transform.pos[1]);
        local.size *= s;
        break;
    default:
        break;
    }
    // Scale stroke/glow with the instance.
    local.stroke.width *= s;
    local.glow.radius *= s;
    return local;
}

std::vector<ArtPrim> expand_prefab(const std::string& prefab,
                                   const json& params,
                                   const Transform& base,
                                   const Palette& palette,
                                   const std::string& pointer) {
    std::vector<ArtPrim> out;
    auto param_f = [&](const char* k, double def) { return double(params.value(k, def)); };
    auto param_color = [&](const char* k, const char* role) {
        const std::string v = params.value(k, std::string(role));
        return resolve_color(v, palette);
    };

    if (prefab == "starburst") {
        // §4.1: star polygon + glow.
        const int spikes = int(param_f("spikes", 8));
        const float r = float(param_f("r", 0.03));
        const float inner = float(param_f("inner_frac", 0.35));
        const uint32_t color = param_color("color", "primary");
        const float gi = float(param_f("glow_intensity", 1.2));
        ArtPrim star;
        star.kind = ArtPrim::Kind::Polygon;
        star.r = r;
        for (int i = 0; i < spikes * 2; ++i) {
            const float rr = (i % 2 == 0) ? r : r * inner;
            const float ang = float(i) * kPi / float(spikes);
            star.points.push_back(rr * std::cos(ang));
            star.points.push_back(rr * std::sin(ang));
        }
        star.stroke = {0.0015f, color};
        star.glow = {r * 0.6f, gi, false, color};
        star.fill.color0 = (color & 0xFFFFFF00u) | 0x26u; // 15% fill
        out.push_back(child_prim(base, star));
        return out;
    }
    if (prefab == "dotted_circle") {
        // §4.7: dots on a radius.
        const float r = float(param_f("r", 0.020));
        const int dots = int(param_f("dots", 12));
        const float dot_r = float(param_f("dot_r", 0.0015));
        const uint32_t color = param_color("color", "primary");
        for (int i = 0; i < dots; ++i) {
            const float ang = float(i) * 2.0f * kPi / float(dots);
            ArtPrim dot;
            dot.kind = ArtPrim::Kind::Circle;
            dot.r = dot_r;
            dot.transform.pos[0] = r * std::cos(ang);
            dot.transform.pos[1] = r * std::sin(ang);
            dot.fill.color0 = color;
            dot.glow = {dot_r * 3.0f, 0.9f, false, color};
            out.push_back(child_prim(base, dot));
        }
        return out;
    }
    if (prefab == "chevron_row") {
        // §4.3: capsule pairs marching along dir_deg.
        const int count = int(param_f("count", 3));
        const float w = float(param_f("w", 0.012));
        const float h = float(param_f("h", 0.010));
        const float gap = float(param_f("gap", 0.004));
        const float dir = float(param_f("dir_deg", 90)) * kPi / 180.0f;
        const uint32_t color = param_color("color", "primary");
        const float gi = float(param_f("glow_intensity", 1.0));
        for (int i = 0; i < count; ++i) {
            const float along = float(i) * (h + gap);
            for (int arm = 0; arm < 2; ++arm) {
                ArtPrim cap;
                cap.kind = ArtPrim::Kind::Capsule;
                cap.r = w * 0.25f;
                // Two arms meeting at the tip, ±40° from dir.
                const float spread = 40.0f * kPi / 180.0f;
                const float arm_ang = dir + (arm == 0 ? spread : -spread);
                const float tip_x = std::cos(dir) * along;
                const float tip_y = std::sin(dir) * along;
                cap.a[0] = tip_x;
                cap.a[1] = tip_y;
                cap.b[0] = tip_x + std::cos(arm_ang) * h;
                cap.b[1] = tip_y + std::sin(arm_ang) * h;
                cap.fill.color0 = (color & 0xFFFFFF00u) | 0x26u;
                cap.stroke = {0.0012f, color};
                cap.glow = {w * 0.4f, gi, false, color};
                out.push_back(child_prim(base, cap));
            }
        }
        return out;
    }
    if (prefab == "lightning_bolt") {
        // §4.9: 7-point zigzag polygon.
        const float h = float(param_f("h", 0.030));
        const float w = float(param_f("w", 0.014));
        const uint32_t color = param_color("color", "warm");
        const float gi = float(param_f("glow_intensity", 1.6));
        ArtPrim poly;
        poly.kind = ArtPrim::Kind::Polygon;
        const float x0 = -w * 0.5f, x1 = w * 0.5f;
        const float y0 = -h * 0.5f, y1 = h * 0.5f;
        const float dy = h / 3.0f;
        poly.points = {x1,
                       y1,
                       x0,
                       y1 - dy,
                       x1 * 0.4f,
                       y1 - dy - dy * 0.2f,
                       x0,
                       y0 + dy,
                       x1 * 0.6f,
                       y0 + dy * 0.6f,
                       x0,
                       y0};
        poly.stroke = {0.0015f, color};
        poly.glow = {w * 0.5f, gi, false, color};
        poly.fill.color0 = (color & 0xFFFFFF00u) | 0x40u;
        out.push_back(child_prim(base, poly));
        return out;
    }
    if (prefab == "tube_outline") {
        // §4.11: double-stroked polyline.
        const float r_tube = float(param_f("r_tube", 0.004));
        const uint32_t color = param_color("color", "secondary");
        std::vector<float> pts;
        if (auto it = params.find("points"); it != params.end() && it->is_array()) {
            for (const auto& pt : *it) {
                if (pt.is_array() && pt.size() == 2) {
                    pts.push_back(float(pt[0].get<double>()));
                    pts.push_back(float(pt[1].get<double>()));
                }
            }
        }
        if (pts.size() < 4) {
            fail("tube_outline needs points", pointer + "/params/points");
        }
        ArtPrim tube;
        tube.kind = ArtPrim::Kind::Polyline;
        tube.points = pts;
        tube.stroke = {2.0f * r_tube, (color & 0xFFFFFF00u) | 0x80u}; // × 0.5
        out.push_back(child_prim(base, tube));
        ArtPrim spec;
        spec.kind = ArtPrim::Kind::Polyline;
        spec.points = pts;
        // Specular edge: glow_white, offset.
        for (size_t i = 0; i < spec.points.size(); i += 2) {
            spec.points[i] += 0.0008f;
            spec.points[i + 1] += 0.0008f;
        }
        spec.stroke = {0.35f * r_tube, palette.rgba[7]};
        out.push_back(child_prim(base, spec));
        return out;
    }
    if (prefab == "grid_horizon") {
        // §4.5: perspective floor.
        const float w = float(param_f("w", 0.40));
        const float h = float(param_f("h", 0.24));
        const int lines = int(param_f("lines", 8));
        const uint32_t color = param_color("color", "primary");
        // Rows shrink geometrically toward the TOP; FAR rows (high i,
        // small y) get color × 0.4 — the perspective depth cue
        // (§4.5 "Top rows use color × 0.4"; cycle-1 fixed the fade
        // and the verticals' convergence below).
        float y = h * 0.5f;
        float spacing = h / float(lines + 1);
        for (int i = 0; i < lines; ++i) {
            spacing *= 0.78f;
            y -= spacing;
            ArtPrim seg;
            seg.kind = ArtPrim::Kind::Segment;
            seg.a[0] = -w * 0.5f;
            seg.a[1] = y;
            seg.b[0] = w * 0.5f;
            seg.b[1] = y;
            const float fade = (lines > 1 ? float(i) / float(lines - 1) : 0.0f) * 0.6f; // 1.0 → 0.4
            const uint32_t c =
                (color & 0xFFFFFF00u) | uint32_t(float(color & 0xFFu) * (1.0f - fade));
            seg.stroke = {0.0012f, c};
            seg.glow = {0.004f, 0.8f, false, c};
            out.push_back(child_prim(base, seg));
        }
        // 7 verticals CONVERGING toward the vanishing point: the top
        // endpoint pulls to center x=0 (cycle-1 — the original
        // diverged outward).
        for (int i = 0; i < 7; ++i) {
            const float x = -w * 0.5f + w * float(i) / 6.0f;
            ArtPrim seg;
            seg.kind = ArtPrim::Kind::Segment;
            seg.a[0] = x;
            seg.a[1] = h * 0.5f;
            seg.b[0] = x * 0.15f;
            seg.b[1] = -h * 0.5f;
            seg.stroke = {0.0012f, (color & 0xFFFFFF00u) | 0x80u};
            out.push_back(child_prim(base, seg));
        }
        return out;
    }

    fail("unknown decal prefab '" + prefab + "'", pointer + "/prefab");
}

ArtPrim parse_prim(const json& p,
                   const Palette& palette,
                   const std::unordered_map<std::string, int>& light_ids) {
    const std::string pointer = ""; // fine-grained pointers via fail() below
    if (!p.is_object()) {
        fail("primitive must be an object", pointer);
    }
    const std::string kind = p.value("kind", std::string());
    ArtPrim prim;

    if (kind == "decal") {
        prim.kind = ArtPrim::Kind::DecalGroup;
        if (!p.contains("transform")) {
            fail("decal missing transform", pointer + "/transform");
        }
        const Transform base = parse_transform(p.at("transform"), pointer + "/transform");
        if (p.contains("prefab") == p.contains("image")) {
            fail("decal needs exactly one of prefab|image", pointer);
        }
        if (p.contains("image")) {
            fail("image decals are out of M13a scope (no raster path yet)", pointer + "/image");
        }
        const json empty = json::object();
        const json& params = p.contains("params") ? p.at("params") : empty;
        if (!params.is_object()) {
            fail("decal params must be an object", pointer + "/params");
        }
        prim.children =
            expand_prefab(p.at("prefab").get<std::string>(), params, base, palette, pointer);
        return prim;
    }

    // All non-decal kinds need a transform.
    if (auto it = p.find("transform"); it != p.end()) {
        prim.transform = parse_transform(*it, pointer + "/transform");
    } else {
        fail("primitive missing transform", pointer);
    }

    if (kind == "circle") {
        prim.kind = ArtPrim::Kind::Circle;
        prim.r = float(p.value("r", 0.0));
        if (!(prim.r > 0.0f)) {
            fail("circle r must be > 0", pointer);
        }
    } else if (kind == "ring") {
        prim.kind = ArtPrim::Kind::Ring;
        prim.r = float(p.value("r", 0.0));
        prim.thickness = float(p.value("thickness", 0.0));
        if (!(prim.r > 0.0f) || !(prim.thickness > 0.0f)) {
            fail("ring r/thickness must be > 0", pointer);
        }
    } else if (kind == "rect") {
        prim.kind = ArtPrim::Kind::Rect;
        prim.w = float(p.value("w", 0.0));
        prim.h = float(p.value("h", 0.0));
        prim.corner_r = float(p.value("corner_r", 0.0));
        if (!(prim.w > 0.0f) || !(prim.h > 0.0f)) {
            fail("rect w/h must be > 0", pointer);
        }
    } else if (kind == "capsule") {
        prim.kind = ArtPrim::Kind::Capsule;
        const json& ja = p.at("a");
        const json& jb = p.at("b");
        if (!ja.is_array() || ja.size() != 2 || !jb.is_array() || jb.size() != 2) {
            fail("capsule a/b must be [x, y]", pointer);
        }
        prim.a[0] = float(ja[0].get<double>());
        prim.a[1] = float(ja[1].get<double>());
        prim.b[0] = float(jb[0].get<double>());
        prim.b[1] = float(jb[1].get<double>());
        prim.r = float(p.value("r", 0.0));
        if (!(prim.r > 0.0f)) {
            fail("capsule r must be > 0", pointer);
        }
    } else if (kind == "segment") {
        prim.kind = ArtPrim::Kind::Segment;
        const json& ja = p.at("a");
        const json& jb = p.at("b");
        prim.a[0] = float(ja[0].get<double>());
        prim.a[1] = float(ja[1].get<double>());
        prim.b[0] = float(jb[0].get<double>());
        prim.b[1] = float(jb[1].get<double>());
    } else if (kind == "polyline" || kind == "polygon") {
        prim.kind = kind == "polyline" ? ArtPrim::Kind::Polyline : ArtPrim::Kind::Polygon;
        const json& pts = p.at("points");
        if (!pts.is_array()) {
            fail("points must be an array", pointer + "/points");
        }
        const size_t min_pts = prim.kind == ArtPrim::Kind::Polyline ? 2 : 3;
        const size_t max_pts = prim.kind == ArtPrim::Kind::Polyline ? 128 : 256;
        if (pts.size() < min_pts || pts.size() > max_pts) {
            fail(kind + " needs " + std::to_string(min_pts) + "-" + std::to_string(max_pts) +
                     " points",
                 pointer + "/points");
        }
        for (const auto& pt : pts) {
            if (!pt.is_array() || pt.size() != 2) {
                fail("point must be [x, y]", pointer + "/points");
            }
            prim.points.push_back(float(pt[0].get<double>()));
            prim.points.push_back(float(pt[1].get<double>()));
        }
        prim.closed = p.value("closed", false);
    } else if (kind == "arc") {
        prim.kind = ArtPrim::Kind::Arc;
        prim.r = float(p.value("r", 0.0));
        prim.thickness = float(p.value("thickness", 0.0));
        prim.start_deg = float(p.value("start_deg", 0.0));
        prim.end_deg = float(p.value("end_deg", 360.0));
        if (!(prim.r > 0.0f) || !(prim.thickness > 0.0f)) {
            fail("arc r/thickness must be > 0", pointer);
        }
        if (!(prim.end_deg > prim.start_deg)) {
            fail("arc end_deg must exceed start_deg", pointer);
        }
        if (prim.end_deg - prim.start_deg > 360.0f) {
            fail("arc span must be <= 360", pointer);
        }
    } else if (kind == "star") {
        // §3.3: expands to a polygon at load.
        prim.kind = ArtPrim::Kind::Polygon;
        const int n = int(p.value("points_n", 5));
        if (n < 4 || n > 12) {
            fail("star points_n must be 4-12", pointer + "/points_n");
        }
        const float r_out = float(p.value("r_outer", 0.0));
        const float r_in = float(p.value("r_inner", 0.0));
        if (!(r_out > 0.0f) || !(r_in > 0.0f)) {
            fail("star radii must be > 0", pointer);
        }
        for (int i = 0; i < n * 2; ++i) {
            const float rr = (i % 2 == 0) ? r_out : r_in;
            const float ang = float(i) * kPi / float(n) - kPi * 0.5f;
            prim.points.push_back(rr * std::cos(ang));
            prim.points.push_back(rr * std::sin(ang));
        }
    } else if (kind == "text") {
        prim.kind = ArtPrim::Kind::Text;
        prim.text = p.value("string", std::string());
        if (prim.text.empty()) {
            fail("text needs a string", pointer + "/string");
        }
        prim.font = font_id(p.value("font", std::string("orbitron")), pointer);
        prim.size = float(p.value("size", 0.0));
        if (!(prim.size > 0.0f)) {
            fail("text size must be > 0", pointer + "/size");
        }
        const std::string align = p.value("align", std::string("left"));
        if (align == "left") {
            prim.align = 0;
        } else if (align == "center") {
            prim.align = 1;
        } else if (align == "right") {
            prim.align = 2;
        } else {
            fail("text align must be left|center|right", pointer + "/align");
        }
        prim.letter_spacing = float(p.value("letter_spacing", 0.0));
    } else {
        fail("unknown primitive kind '" + kind + "'", pointer + "/kind");
    }

    // Paint + stroke + glow + light.
    if (auto it = p.find("fill"); it != p.end() && prim.kind != ArtPrim::Kind::Segment &&
                                  prim.kind != ArtPrim::Kind::Polyline) {
        prim.fill = parse_fill(*it, palette, pointer + "/fill");
    }
    if (auto it = p.find("stroke"); it != p.end() && it->is_object()) {
        const double width = it->value("width", 0.0);
        if (!(width >= 0.0)) {
            fail("stroke.width must be >= 0", pointer + "/stroke/width");
        }
        prim.stroke.width = float(width);
        prim.stroke.color = resolve_color(it->value("color", std::string("primary")), palette);
    }
    if (auto it = p.find("glow"); it != p.end() && it->is_object()) {
        prim.glow = parse_glow(*it, palette, pointer + "/glow");
    }
    if (auto it = p.find("light"); it != p.end() && it->is_string()) {
        const std::string id = it->get<std::string>();
        const auto found = light_ids.find(id);
        if (found == light_ids.end()) {
            fail("light id '" + id + "' does not exist in table.json", pointer + "/light");
        }
        prim.light_index = found->second;
    }
    return prim;
}

} // namespace

ArtLoadResult load_art(const std::filesystem::path& dir,
                       const std::vector<std::pair<std::string, int>>& light_ids_in) {
    ArtLoadResult result;
    const std::filesystem::path file = dir / "art.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return result; // no art: greybox
    }
    std::ifstream in(file);
    if (!in.good()) {
        TB_LOG_WARN("render", "art.json exists but cannot be read");
        return result;
    }
    json doc;
    try {
        doc = json::parse(in, nullptr, true, true); // comments allowed
    } catch (const json::parse_error& e) {
        fail(std::string("parse error: ") + e.what(), "");
    }
    if (!doc.is_object()) {
        fail("root must be an object", "");
    }

    // Palette: canon name or a custom 8-role object (§3.1).
    TbArt art;
    if (auto it = doc.find("palette"); it != doc.end() && it->is_string()) {
        const std::string name = it->get<std::string>();
        const Palette* p = canon_palette(name);
        if (p == nullptr) {
            fail("unknown palette '" + name + "'", "/palette");
        }
        art.palette_name = name;
        art.palette = *p;
    } else if (it != doc.end() && it->is_object()) {
        for (const char* role :
             {"bg0", "bg1", "primary", "secondary", "accent1", "accent2", "warm", "glow_white"}) {
            if (auto r = it->find(role); r == it->end() || !r->is_string()) {
                fail("custom palette must supply all 8 roles", "/palette");
            }
            art.palette.rgba[size_t(palette_role(role))] =
                parse_hex_color(it->at(role).get<std::string>(), "/palette");
        }
        art.palette_name = "custom";
        art.palette.name = "custom";
    } else {
        fail("palette must be a canon name or an 8-role object", "/palette");
    }

    if (auto ball = doc.find("ball"); ball != doc.end() && ball->is_object()) {
        art.ball_trail = ball->value("trail", false);
        art.ball_trail_color =
            resolve_color(ball->value("trail_color", std::string("glow_white")), art.palette);
    }

    // Light binding map: element-id string -> art light index.
    std::unordered_map<std::string, int> light_ids;
    for (const auto& [id, idx] : light_ids_in) {
        light_ids.emplace(id, idx);
    }

    const json& layers = doc.at("layers");
    if (!layers.is_array() || layers.empty() || layers.size() > 32) {
        fail("layers must be 1-32 entries", "/layers");
    }
    for (const json& lj : layers) {
        ArtLayer layer;
        layer.name = lj.value("name", std::string());
        layer.z = int(lj.value("z", 0));
        if (layer.z < 0 || layer.z > 199) {
            fail("layer z must be 0-199", "/layers");
        }
        const std::string blend = lj.value("blend", std::string("normal"));
        if (blend != "normal" && blend != "additive") {
            fail("layer blend must be normal|additive", "/layers");
        }
        layer.additive = blend == "additive";
        if (auto prims = lj.find("primitives"); prims != lj.end() && prims->is_array()) {
            for (const json& pj : *prims) {
                layer.prims.push_back(parse_prim(pj, art.palette, light_ids));
            }
        }
        art.layers.push_back(std::move(layer));
    }
    // Unique z per layer (validated).
    for (size_t i = 0; i < art.layers.size(); ++i) {
        for (size_t j = i + 1; j < art.layers.size(); ++j) {
            if (art.layers[i].z == art.layers[j].z) {
                fail("duplicate layer z " + std::to_string(art.layers[i].z), "/layers");
            }
        }
    }

    result.loaded = true;
    result.art = std::move(art);
    return result;
}

} // namespace tb::render
