#include "table/table_loader.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>

namespace tb::table {

namespace {

using json = nlohmann::json;

constexpr float kPi = 3.14159265358979f;

[[noreturn]] void
fail(const std::string& what, const std::string& pointer, const std::filesystem::path& file) {
    throw TableLoadError(what + " (at " + pointer + ")", pointer, file);
}

float get_number(const json& obj,
                 const char* key,
                 float fallback,
                 const std::string& pointer,
                 const std::filesystem::path& file) {
    if (!obj.contains(key)) {
        return fallback;
    }
    const json& v = obj.at(key);
    if (!v.is_number()) {
        fail(std::string("field '") + key + "' must be a number", pointer + "/" + key, file);
    }
    return v.get<float>();
}

std::string get_string(const json& obj,
                       const char* key,
                       const std::string& fallback,
                       const std::string& pointer,
                       const std::filesystem::path& file) {
    if (!obj.contains(key)) {
        return fallback;
    }
    const json& v = obj.at(key);
    if (!v.is_string()) {
        fail(std::string("field '") + key + "' must be a string", pointer + "/" + key, file);
    }
    return v.get<std::string>();
}

void get_xy(const json& obj,
            const char* key,
            float out[2],
            const std::string& pointer,
            const std::filesystem::path& file) {
    if (!obj.contains(key)) {
        fail(std::string("missing required field '") + key + "'", pointer + "/" + key, file);
    }
    const json& v = obj.at(key);
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
        fail(std::string("field '") + key + "' must be [x, y]", pointer + "/" + key, file);
    }
    out[0] = v[0].get<float>();
    out[1] = v[1].get<float>();
}

bool get_bool(const json& obj,
              const char* key,
              bool fallback,
              const std::string& pointer,
              const std::filesystem::path& file) {
    if (!obj.contains(key)) {
        return fallback;
    }
    if (!obj.at(key).is_boolean()) {
        fail(std::string("field '") + key + "' must be a boolean", pointer + "/" + key, file);
    }
    return obj.at(key).get<bool>();
}

int get_int(const json& obj,
            const char* key,
            int fallback,
            float lo,
            float hi,
            const std::string& pointer,
            const std::filesystem::path& file) {
    if (!obj.contains(key)) {
        return fallback;
    }
    const json& v = obj.at(key);
    if (!v.is_number()) {
        fail(std::string("field '") + key + "' must be an integer", pointer + "/" + key, file);
    }
    const float f = v.get<float>();
    if (f != std::floor(f) || f < lo || f > hi) {
        fail(std::string("field '") + key + "' must be an integer in [" + std::to_string(int(lo)) +
                 ", " + std::to_string(int(hi)) + "]",
             pointer + "/" + key,
             file);
    }
    return int(f);
}

std::vector<std::string> get_tags(const json& obj) {
    std::vector<std::string> tags;
    if (obj.contains("tags") && obj.at("tags").is_array()) {
        for (const auto& t : obj.at("tags")) {
            if (t.is_string()) {
                tags.push_back(t.get<std::string>());
            }
        }
    }
    return tags;
}

MaterialId parse_material(const std::string& name,
                          const std::string& pointer,
                          const std::filesystem::path& file) {
    if (name == "wood") {
        return MaterialId::Wood;
    }
    if (name == "steel") {
        return MaterialId::Steel;
    }
    if (name == "rubber") {
        return MaterialId::Rubber;
    }
    if (name == "plastic") {
        return MaterialId::Plastic;
    }
    fail("unknown material '" + name + "'", pointer, file);
}

std::vector<PathNode>
parse_path(const json& obj, const std::string& pointer, const std::filesystem::path& file) {
    if (!obj.contains("path")) {
        fail("missing required field 'path'", pointer, file);
    }
    const json& arr = obj.at("path");
    if (!arr.is_array() || arr.size() < 2 || arr.size() > 256) {
        fail("field 'path' must hold 2–256 nodes", pointer + "/path", file);
    }
    std::vector<PathNode> nodes;
    float cur[2] = {0.0f, 0.0f}; // running position (points and arc ends)
    for (size_t i = 0; i < arr.size(); ++i) {
        const json& n = arr[i];
        const std::string node_ptr = pointer + "/path/" + std::to_string(i);
        auto update_cur = [&cur](const PathNode& node) {
            if (node.is_arc) {
                cur[0] = node.arc.to[0];
                cur[1] = node.arc.to[1];
            } else {
                cur[0] = node.point[0];
                cur[1] = node.point[1];
            }
        };
        if (n.is_array() && n.size() == 2 && n[0].is_number() && n[1].is_number()) {
            PathNode node;
            node.is_arc = false;
            node.point[0] = n[0].get<float>();
            node.point[1] = n[1].get<float>();
            nodes.push_back(node);
            update_cur(node);
            continue;
        }
        if (n.is_object() && n.contains("arc")) {
            if (nodes.empty()) {
                fail("path must start with a point [x, y] before any arc", node_ptr, file);
            }
            if (i == 0) {
                fail("first path node must be a point", node_ptr, file);
            }
            const json& arc = n.at("arc");
            if (!arc.is_object() || !arc.contains("to") || !arc.contains("radius") ||
                !arc.contains("dir")) {
                fail("arc node needs {to, radius, dir}", node_ptr, file);
            }
            PathNode node;
            node.is_arc = true;
            const json& to = arc.at("to");
            if (!to.is_array() || to.size() != 2 || !to[0].is_number() || !to[1].is_number()) {
                fail("arc 'to' must be [x, y]", node_ptr + "/arc/to", file);
            }
            node.arc.to[0] = to[0].get<float>();
            node.arc.to[1] = to[1].get<float>();
            if (!arc.at("radius").is_number() || !arc.at("dir").is_string()) {
                fail("arc 'radius' must be a number and 'dir' must be a string",
                     node_ptr + "/arc",
                     file);
            }
            node.arc.radius = arc.at("radius").get<float>();
            const std::string dir = arc.at("dir").get<std::string>();
            if (dir != "cw" && dir != "ccw") {
                fail("arc 'dir' must be \"cw\" or \"ccw\"", node_ptr + "/arc/dir", file);
            }
            // §3 feasibility: arcs are minor; |to - from| >= 0.001 and
            // radius >= half-chord (V019). `cur` is the running position
            // (the previous node may itself be an arc).
            const float dx = node.arc.to[0] - cur[0];
            const float dy = node.arc.to[1] - cur[1];
            const float chord = std::sqrt(dx * dx + dy * dy);
            if (chord < 0.001f) {
                fail("degenerate arc: |to - from| < 0.001 m (V019)", node_ptr, file);
            }
            if (node.arc.radius < chord * 0.5f) {
                fail("arc radius < |to - from| / 2 (V019)", node_ptr + "/arc/radius", file);
            }
            node.arc.cw = dir == "cw";
            nodes.push_back(node);
            update_cur(node);
            continue;
        }
        fail("path node must be [x, y] or {\"arc\": {...}}", node_ptr, file);
    }
    return nodes;
}

Element parse_element(const json& obj, size_t index, const std::filesystem::path& file) {
    const std::string pointer = "/elements/" + std::to_string(index);
    if (!obj.is_object()) {
        fail("element must be an object", pointer, file);
    }
    const std::string id = get_string(obj, "id", "", pointer, file);
    if (id.empty()) {
        fail("missing required field 'id'", pointer, file);
    }
    const std::string type = get_string(obj, "type", "", pointer, file);
    const int layer = get_int(obj, "layer", 0, 0, 1, pointer, file);
    const auto tags = get_tags(obj);

    if (type == "wall") {
        WallDef w;
        w.id = id;
        w.layer = layer;
        w.tags = tags;
        w.path = parse_path(obj, pointer, file);
        w.closed = get_bool(obj, "closed", false, pointer, file);
        w.material =
            parse_material(get_string(obj, "material", "wood", pointer, file), pointer, file);
        return Element{std::move(w)};
    }
    if (type == "post") {
        PostDef p;
        p.id = id;
        p.layer = layer;
        p.tags = tags;
        get_xy(obj, "pos", p.pos, pointer, file);
        p.radius = get_number(obj, "radius", 0.008f, pointer, file);
        p.material =
            parse_material(get_string(obj, "material", "rubber", pointer, file), pointer, file);
        return Element{std::move(p)};
    }
    if (type == "flipper") {
        FlipperDef f;
        f.id = id;
        f.layer = layer;
        f.tags = tags;
        get_xy(obj, "pos", f.pos, pointer, file);
        f.length = get_number(obj, "length", 0.076f, pointer, file);
        f.radius_base = get_number(obj, "radius_base", 0.011f, pointer, file);
        f.radius_tip = get_number(obj, "radius_tip", 0.007f, pointer, file);
        if (!obj.contains("rest_angle_deg")) {
            fail("missing required field 'rest_angle_deg'", pointer + "/rest_angle_deg", file);
        }
        f.rest_angle_deg = get_number(obj, "rest_angle_deg", 0.0f, pointer, file);
        f.swing_deg = get_number(obj, "swing_deg", 52.0f, pointer, file);
        const std::string side = get_string(obj, "side", "", pointer, file);
        if (side != "left" && side != "right") {
            fail("flipper 'side' must be \"left\" or \"right\"", pointer + "/side", file);
        }
        f.left_side = side == "left";
        f.input = get_string(obj, "input", side, pointer, file);
        if (f.input != "left" && f.input != "right" && f.input != "upper_left" &&
            f.input != "upper_right") {
            fail("flipper 'input' must be left/right/upper_left/upper_right",
                 pointer + "/input",
                 file);
        }
        f.strength = get_number(obj, "strength", 1.0f, pointer, file);
        return Element{std::move(f)};
    }
    if (type == "plunger") {
        PlungerDef p;
        p.id = id;
        get_xy(obj, "pos", p.pos, pointer, file);
        p.launch_angle_deg = get_number(obj, "launch_angle_deg", 90.0f, pointer, file);
        p.max_speed = get_number(obj, "max_speed", 7.5f, pointer, file);
        p.charge_time_s = get_number(obj, "charge_time_s", 1.5f, pointer, file);
        p.auto_launch = get_bool(obj, "auto", false, pointer, file);
        p.auto_delay_ms = get_number(obj, "auto_delay_ms", 500.0f, pointer, file);
        return Element{std::move(p)};
    }
    if (type == "outhole") {
        OutholeDef o;
        o.id = id;
        if (!obj.contains("region") || !obj.at("region").is_object()) {
            fail("missing required field 'region' {a, b}", pointer, file);
        }
        const json& region = obj.at("region");
        get_xy(region, "a", o.a, pointer + "/region", file);
        get_xy(region, "b", o.b, pointer + "/region", file);
        return Element{std::move(o)};
    }
    if (type == "trough") {
        TroughDef t;
        t.id = id;
        t.capacity = get_int(obj, "capacity", 4, 1, 6, pointer, file);
        return Element{std::move(t)};
    }
    if (type == "light") {
        LightDef l;
        l.id = id;
        l.layer = layer;
        l.tags = tags;
        get_xy(obj, "pos", l.pos, pointer, file);
        l.shape = get_string(obj, "shape", "circle", pointer, file);
        l.size = get_number(obj, "size", 0.012f, pointer, file);
        l.color = get_string(obj, "color", "", pointer, file);
        if (l.color.empty()) {
            fail("missing required field 'color'", pointer, file);
        }
        l.direction_deg = get_number(obj, "direction_deg", 90.0f, pointer, file);
        return Element{std::move(l)};
    }
    if (type == "gate") {
        GateDef g;
        g.id = id;
        g.layer = layer;
        get_xy(obj, "pos", g.pos, pointer, file);
        g.width = get_number(obj, "width", 0.040f, pointer, file);
        g.facing_deg = get_number(obj, "facing_deg", 90.0f, pointer, file);
        const std::string state = get_string(obj, "default_state", "one_way", pointer, file);
        if (state != "one_way" && state != "open" && state != "closed") {
            fail("gate 'default_state' must be one_way/open/closed",
                 pointer + "/default_state",
                 file);
        }
        g.state_open = state == "open";
        g.state_closed = state == "closed";
        return Element{std::move(g)};
    }
    if (type == "rollover") {
        RolloverDef r;
        r.id = id;
        r.layer = layer;
        get_xy(obj, "pos", r.pos, pointer, file);
        r.facing_deg = get_number(obj, "facing_deg", 90.0f, pointer, file);
        return Element{std::move(r)};
    }
    if (type == "slingshot") {
        SlingshotDef sl;
        sl.id = id;
        sl.layer = layer;
        sl.tags = tags;
        if (!obj.contains("face") || !obj.at("face").is_array() || obj.at("face").size() != 2) {
            fail("missing required field 'face' [[x,y],[x,y]]", pointer, file);
        }
        const json& face = obj.at("face");
        if (!face[0].is_array() || face[0].size() != 2 || !face[1].is_array() ||
            face[1].size() != 2 || !face[0][0].is_number() || !face[0][1].is_number() ||
            !face[1][0].is_number() || !face[1][1].is_number()) {
            fail("field 'face' must be [[x,y],[x,y]]", pointer + "/face", file);
        }
        sl.face_a[0] = face[0][0].get<float>();
        sl.face_a[1] = face[0][1].get<float>();
        sl.face_b[0] = face[1][0].get<float>();
        sl.face_b[1] = face[1][1].get<float>();
        sl.kick_speed = get_number(obj, "kick_speed", 3.5f, pointer, file);
        sl.cooldown_ms = get_number(obj, "cooldown_ms", 80.0f, pointer, file);
        const float fx = sl.face_b[0] - sl.face_a[0];
        const float fy = sl.face_b[1] - sl.face_a[1];
        const float flen = std::sqrt(fx * fx + fy * fy);
        if (flen < 0.040f || flen > 0.100f) {
            fail("slingshot 'face' length must be in [0.040, 0.100] (§4.6)",
                 pointer + "/face",
                 file);
        }
        return Element{std::move(sl)};
    }
    if (type == "spinner") {
        SpinnerDef sp;
        sp.id = id;
        sp.layer = layer;
        get_xy(obj, "pos", sp.pos, pointer, file);
        sp.facing_deg = get_number(obj, "facing_deg", 90.0f, pointer, file);
        return Element{std::move(sp)};
    }
    if (type == "ramp") {
        RampDef r;
        r.id = id;
        r.layer = layer;
        r.path = parse_path(obj, pointer, file);
        r.width = get_number(obj, "width", 0.044f, pointer, file);
        if (!obj.contains("height_profile") || !obj.at("height_profile").is_array()) {
            fail("missing required field 'height_profile'", pointer, file);
        }
        for (const auto& k : obj.at("height_profile")) {
            if (!k.is_object() || !k.contains("s") || !k.contains("z") || !k.at("s").is_number() ||
                !k.at("z").is_number()) {
                fail("height_profile keyframes must be {s, z} numbers",
                     pointer + "/height_profile",
                     file);
            }
            r.height_profile.push_back(
                {k.at("s").get<float>(), std::max(0.0f, k.at("z").get<float>())});
        }
        r.drop_exit = get_bool(obj, "drop_exit", false, pointer, file);

        // V010: profile shape; V011: end z must land on a surface.
        const auto& kp = r.height_profile;
        if (kp.empty() || std::fabs(kp.front().s) > 1e-6f ||
            std::fabs(kp.back().s - 1.0f) > 1e-6f) {
            fail("height_profile must start at s:0 and end at s:1 (V010)",
                 pointer + "/height_profile",
                 file);
        }
        for (size_t i = 1; i < kp.size(); ++i) {
            if (kp[i].s <= kp[i - 1].s) {
                fail("height_profile s must strictly increase (V010)",
                     pointer + "/height_profile",
                     file);
            }
            if (kp[i].z > 0.15f || kp[i].z < 0.0f) {
                fail("height_profile z must be in [0, 0.15] (V010)",
                     pointer + "/height_profile",
                     file);
            }
        }
        if (!r.drop_exit) {
            const float layer1_z = 0.055f; // playfield.layer1_z default
            const float z_end = kp.back().z;
            const bool surf0 = std::fabs(z_end) <= 0.005f;
            const bool surf1 = std::fabs(z_end - layer1_z) <= 0.005f;
            if (!surf0 && !surf1) {
                fail("ramp end z must be 0 or layer1_z (±0.005) unless "
                     "drop_exit (V011)",
                     pointer + "/height_profile",
                     file);
            }
        }
        return Element{std::move(r)};
    }
    if (type == "magnet") {
        MagnetDef m;
        m.id = id;
        m.layer = layer;
        get_xy(obj, "pos", m.pos, pointer, file);
        m.radius = get_number(obj, "radius", 0.09f, pointer, file);
        m.strength = get_number(obj, "strength", 1.2f, pointer, file);
        m.default_on = get_bool(obj, "default_on", false, pointer, file);
        return Element{std::move(m)};
    }
    if (type == "kicker") {
        KickerDef k;
        k.id = id;
        k.layer = layer;
        get_xy(obj, "pos", k.pos, pointer, file);
        k.radius = get_number(obj, "radius", 0.014f, pointer, file);
        if (k.radius <= 0.0f) {
            fail("kicker 'radius' must be > 0", pointer + "/radius", file);
        }
        k.style = get_string(obj, "style", "saucer", pointer, file);
        if (k.style != "saucer" && k.style != "scoop" && k.style != "vuk") {
            fail("kicker 'style' must be saucer/scoop/vuk", pointer + "/style", file);
        }
        k.capture_ms = get_number(obj, "capture_ms", 800.0f, pointer, file);
        if (k.capture_ms < 0.0f) {
            fail("kicker 'capture_ms' must be >= 0", pointer + "/capture_ms", file);
        }
        k.eject_speed = get_number(obj, "eject_speed", 3.0f, pointer, file);
        if (!std::isfinite(k.eject_speed) || k.eject_speed <= 0.0f) {
            fail("kicker 'eject_speed' must be finite and > 0", pointer + "/eject_speed", file);
        }
        k.eject_angle_deg = get_number(obj, "eject_angle_deg", 90.0f, pointer, file);
        return Element{std::move(k)};
    }
    if (type == "drop_target_bank") {
        DropTargetBankDef bank;
        bank.id = id;
        bank.layer = layer;
        if (!obj.contains("targets") || !obj.at("targets").is_array()) {
            fail("missing required field 'targets' (2–7 entries)", pointer, file);
        }
        const json& arr = obj.at("targets");
        if (arr.size() < 2 || arr.size() > 7) {
            fail("field 'targets' must hold 2–7 entries", pointer + "/targets", file);
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const std::string tptr = pointer + "/targets/" + std::to_string(i);
            const json& t = arr[i];
            if (!t.is_object()) {
                fail("target entry must be an object", tptr, file);
            }
            DropTargetBankDef::TargetDef td;
            get_xy(t, "pos", td.pos, tptr, file);
            td.width = get_number(t, "width", 0.025f, tptr, file);
            if (!t.contains("facing_deg")) {
                fail("missing required field 'facing_deg'", tptr + "/facing_deg", file);
            }
            td.facing_deg = get_number(t, "facing_deg", 0.0f, tptr, file);
            bank.targets.push_back(td);
        }
        const std::string reset = get_string(obj, "reset", "script", pointer, file);
        if (reset != "script" && reset != "auto") {
            fail("bank 'reset' must be script/auto", pointer + "/reset", file);
        }
        bank.auto_reset = reset == "auto";
        bank.auto_reset_ms = get_number(obj, "auto_reset_ms", 1500.0f, pointer, file);
        return Element{std::move(bank)};
    }
    if (type == "captive_ball") {
        CaptiveBallDef cap;
        cap.id = id;
        cap.layer = layer;
        if (!obj.contains("slot") || !obj.at("slot").is_object()) {
            fail("missing required field 'slot' {a, b}", pointer, file);
        }
        get_xy(obj.at("slot"), "a", cap.a, pointer + "/slot", file);
        get_xy(obj.at("slot"), "b", cap.b, pointer + "/slot", file);
        const float sdx = cap.b[0] - cap.a[0];
        const float sdy = cap.b[1] - cap.a[1];
        const float slot_len = std::sqrt(sdx * sdx + sdy * sdy);
        if (slot_len < 0.040f || slot_len > 0.120f) {
            fail("captive_ball 'slot' length must be in [0.040, 0.120] (§4.15)",
                 pointer + "/slot",
                 file);
        }
        return Element{std::move(cap)};
    }
    if (type == "ball_lock") {
        BallLockDef lock;
        lock.id = id;
        lock.layer = layer;
        get_xy(obj, "pos", lock.pos, pointer, file);
        lock.capacity = get_int(obj, "capacity", 2, 1, 6, pointer, file);
        lock.eject_speed = get_number(obj, "eject_speed", 2.5f, pointer, file);
        if (!std::isfinite(lock.eject_speed) || lock.eject_speed <= 0.0f) {
            fail("ball_lock 'eject_speed' must be finite and > 0", pointer + "/eject_speed", file);
        }
        lock.eject_angle_deg = get_number(obj, "eject_angle_deg", -90.0f, pointer, file);
        return Element{std::move(lock)};
    }
    if (type == "pop_bumper") {
        PopBumperDef p;
        p.id = id;
        p.layer = layer;
        p.tags = tags;
        get_xy(obj, "pos", p.pos, pointer, file);
        p.radius = get_number(obj, "radius", 0.031f, pointer, file);
        p.kick_speed = get_number(obj, "kick_speed", 4.5f, pointer, file);
        p.cooldown_ms = get_number(obj, "cooldown_ms", 60.0f, pointer, file);
        return Element{std::move(p)};
    }
    if (type == "standup_target") {
        StandupTargetDef t;
        t.id = id;
        t.layer = layer;
        t.tags = tags;
        get_xy(obj, "pos", t.pos, pointer, file);
        t.width = get_number(obj, "width", 0.025f, pointer, file);
        if (!obj.contains("facing_deg")) {
            fail("missing required field 'facing_deg'", pointer + "/facing_deg", file);
        }
        t.facing_deg = get_number(obj, "facing_deg", 0.0f, pointer, file);
        t.min_speed = get_number(obj, "min_speed", 0.3f, pointer, file);
        return Element{std::move(t)};
    }
    fail("element type '" + type +
             "' is not supported by this milestone (M5: "
             "wall, post, flipper, plunger, outhole, trough, light)",
         pointer + "/type",
         file);
}

struct PrefabParams {
    std::string id;
    std::string prefab;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{0.0f, 0.0f};
    // Per-prefab params with §5 defaults (see PrefabInstance).
    float tip_gap = 0.068f;
    float length = 0.076f;
    float rest_slope_deg = 31.0f;
    float swing_deg = 52.0f;
    float strength = 1.0f;
    float lane_width = 0.040f;
    float top_y = 0.880f;
    bool auto_launch = false;
    float max_speed = 7.5f;
    float charge_time_s = 1.5f;
    std::string side = "left";
    float mirror_axis_x = 0.240f;
    float mouth_x = 0.075f;
    float top_radius = 0.130f;
    float entry_y_left = 0.550f;
    float entry_y_right = 0.900f;
    bool merged_lane = false;
    float sling_spread = 0.150f;
    float sling_face_length = 0.070f;
    float sling_tilt_deg = 22.0f;
    float sling_kick_speed = 3.5f;
};

PrefabParams parse_prefab(const json& obj, size_t index, const std::filesystem::path& file) {
    const std::string pointer = "/prefabs/" + std::to_string(index);
    PrefabParams p;
    p.id = get_string(obj, "id", "", pointer, file);
    if (p.id.empty()) {
        fail("missing required field 'id'", pointer, file);
    }
    p.prefab = get_string(obj, "prefab", "", pointer, file);
    p.layer = get_int(obj, "layer", 0, 0, 1, pointer, file);
    p.tags = get_tags(obj);
    if (obj.contains("pos")) {
        get_xy(obj, "pos", p.pos, pointer, file);
    }
    p.tip_gap = get_number(obj, "tip_gap", p.tip_gap, pointer, file);
    p.length = get_number(obj, "length", p.length, pointer, file);
    p.rest_slope_deg = get_number(obj, "rest_slope_deg", p.rest_slope_deg, pointer, file);
    p.swing_deg = get_number(obj, "swing_deg", p.swing_deg, pointer, file);
    p.strength = get_number(obj, "strength", p.strength, pointer, file);
    p.lane_width = get_number(obj, "lane_width", p.lane_width, pointer, file);
    p.top_y = get_number(obj, "top_y", p.top_y, pointer, file);
    p.auto_launch = obj.value("auto", false);
    p.max_speed = get_number(obj, "max_speed", p.max_speed, pointer, file);
    p.charge_time_s = get_number(obj, "charge_time_s", p.charge_time_s, pointer, file);
    p.side = get_string(obj, "side", "left", pointer, file);
    p.mirror_axis_x = get_number(obj, "mirror_axis_x", p.mirror_axis_x, pointer, file);
    p.mouth_x = get_number(obj, "mouth_x", p.mouth_x, pointer, file);
    p.top_radius = get_number(obj, "top_radius", p.top_radius, pointer, file);
    p.entry_y_left = get_number(obj, "entry_y_left", p.entry_y_left, pointer, file);
    p.entry_y_right = get_number(obj, "entry_y_right", p.entry_y_right, pointer, file);
    p.sling_spread = get_number(obj, "spread", p.sling_spread, pointer, file);
    p.sling_face_length = get_number(obj, "face_length", p.sling_face_length, pointer, file);
    p.sling_tilt_deg = get_number(obj, "tilt_deg", p.sling_tilt_deg, pointer, file);
    p.sling_kick_speed = get_number(obj, "kick_speed", p.sling_kick_speed, pointer, file);
    return p;
}

PrefabInstance to_instance(const PrefabParams& p) {
    PrefabInstance inst;
    inst.id = p.id;
    inst.prefab = p.prefab;
    inst.layer = p.layer;
    inst.tags = p.tags;
    inst.pos[0] = p.pos[0];
    inst.pos[1] = p.pos[1];
    inst.tip_gap = p.tip_gap;
    inst.length = p.length;
    inst.rest_slope_deg = p.rest_slope_deg;
    inst.swing_deg = p.swing_deg;
    inst.strength = p.strength;
    inst.lane_width = p.lane_width;
    inst.top_y = p.top_y;
    inst.auto_launch = p.auto_launch;
    inst.max_speed = p.max_speed;
    inst.charge_time_s = p.charge_time_s;
    inst.right_side = p.side == "right";
    inst.mirror_axis_x = p.mirror_axis_x;
    inst.mouth_x = p.mouth_x;
    inst.top_radius = p.top_radius;
    inst.entry_y_left = p.entry_y_left;
    inst.entry_y_right = p.entry_y_right;
    inst.merged_with_orbit = p.merged_lane;
    inst.sling_spread = p.sling_spread;
    inst.sling_face_length = p.sling_face_length;
    inst.sling_tilt_deg = p.sling_tilt_deg;
    inst.sling_kick_speed = p.sling_kick_speed;
    return inst;
}

WallDef make_wall(std::string id,
                  int layer,
                  std::vector<std::string> tags,
                  std::vector<PathNode> path,
                  MaterialId material,
                  bool closed = false) {
    WallDef w;
    w.id = std::move(id);
    w.layer = layer;
    w.tags = std::move(tags);
    w.path = std::move(path);
    w.material = material;
    w.closed = closed;
    return w;
}

PathNode point(float x, float y) {
    PathNode n;
    n.is_arc = false;
    n.point[0] = x;
    n.point[1] = y;
    return n;
}

PathNode arc(float to_x, float to_y, float radius, bool cw) {
    PathNode n;
    n.is_arc = true;
    n.arc.to[0] = to_x;
    n.arc.to[1] = to_y;
    n.arc.radius = radius;
    n.arc.cw = cw;
    return n;
}

} // namespace

const std::string& Element::id() const {
    return std::visit([](const auto& d) -> const std::string& { return d.id; }, def);
}

const char* Element::type_name() const {
    return std::visit(
        [](const auto& d) -> const char* {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, WallDef>) {
                return "wall";
            } else if constexpr (std::is_same_v<T, PostDef>) {
                return "post";
            } else if constexpr (std::is_same_v<T, FlipperDef>) {
                return "flipper";
            } else if constexpr (std::is_same_v<T, PlungerDef>) {
                return "plunger";
            } else if constexpr (std::is_same_v<T, OutholeDef>) {
                return "outhole";
            } else if constexpr (std::is_same_v<T, TroughDef>) {
                return "trough";
            } else if constexpr (std::is_same_v<T, LightDef>) {
                return "light";
            } else if constexpr (std::is_same_v<T, GateDef>) {
                return "gate";
            } else if constexpr (std::is_same_v<T, RolloverDef>) {
                return "rollover";
            } else if constexpr (std::is_same_v<T, SlingshotDef>) {
                return "slingshot";
            } else if constexpr (std::is_same_v<T, PopBumperDef>) {
                return "pop_bumper";
            } else if constexpr (std::is_same_v<T, StandupTargetDef>) {
                return "standup_target";
            } else {
                return "?";
            }
        },
        def);
}

std::vector<Element> expand_prefab(const TableDef& partial, const PrefabInstance& inst) {
    std::vector<Element> out;
    const std::string& pid = inst.id;

    if (inst.prefab == "flipper_pair_standard") {
        // §5.1: pivot separation D = tip_gap + 2·length·cos(rest_slope).
        const float d =
            inst.tip_gap + 2.0f * inst.length * std::cos(inst.rest_slope_deg * kPi / 180.0f);
        FlipperDef left;
        left.id = pid + "_left_flipper";
        left.layer = inst.layer;
        left.tags = inst.tags;
        left.pos[0] = inst.pos[0] - d / 2.0f;
        left.pos[1] = inst.pos[1];
        left.length = inst.length;
        left.radius_base = 0.011f;
        left.radius_tip = 0.007f;
        left.rest_angle_deg = -inst.rest_slope_deg;
        left.swing_deg = inst.swing_deg;
        left.left_side = true;
        left.input = "left";
        left.strength = inst.strength;

        FlipperDef right;
        right.id = pid + "_right_flipper";
        right.layer = inst.layer;
        right.tags = inst.tags;
        right.pos[0] = inst.pos[0] + d / 2.0f;
        right.pos[1] = inst.pos[1];
        right.length = inst.length;
        right.radius_base = 0.011f;
        right.radius_tip = 0.007f;
        right.rest_angle_deg = inst.rest_slope_deg - 180.0f;
        right.swing_deg = inst.swing_deg;
        right.left_side = false;
        right.input = "right";
        right.strength = inst.strength;

        out.push_back(Element{std::move(left)});
        out.push_back(Element{std::move(right)});
        return out;
    }

    if (inst.prefab == "plunger_lane") {
        // §5.2: right-side shooter lane. The merged variant (no top post)
        // is decided by the loader — order-independently, by measuring the
        // orbit right mouth against this lane's wall top node — and passed
        // in via merged_with_orbit (see load_table).
        const float w = partial.width;
        const float xw = w - inst.lane_width;
        const float y_gate = inst.top_y + 0.008f;
        const bool merged = inst.merged_with_orbit;

        out.push_back(Element{make_wall(pid + "_wall",
                                        inst.layer,
                                        inst.tags,
                                        {point(xw, 0.0f), point(xw, y_gate)},
                                        MaterialId::Wood)});
        if (!merged) {
            PostDef post;
            post.id = pid + "_top_post";
            post.layer = inst.layer;
            post.tags = inst.tags;
            post.pos[0] = xw - 0.008f;
            post.pos[1] = y_gate;
            post.radius = 0.008f;
            post.material = MaterialId::Rubber;
            out.push_back(Element{std::move(post)});
        }
        GateDef gate;
        gate.id = pid + "_gate";
        gate.layer = inst.layer;
        gate.pos[0] = w - inst.lane_width / 2.0f;
        gate.pos[1] = y_gate;
        gate.width = inst.lane_width;
        gate.facing_deg = 90.0f;
        gate.state_open = false; // shooter-lane one-way exit gate
        out.push_back(Element{std::move(gate)});

        PlungerDef pl;
        pl.id = pid + "_plunger";
        pl.pos[0] = w - inst.lane_width / 2.0f;
        pl.pos[1] = 0.030f;
        pl.launch_angle_deg = 90.0f;
        pl.max_speed = inst.max_speed;
        pl.charge_time_s = inst.charge_time_s;
        pl.auto_launch = inst.auto_launch;
        out.push_back(Element{std::move(pl)});
        return out;
    }

    if (inst.prefab == "sling_pair") {
        // §5.3: two rubber triangles + kicking faces. Left-side geometry
        // per the spec; the right side mirrors about pos.x. Defaults:
        // spread 0.150, face_length 0.070, tilt 22 deg, kick 3.5.
        const float spread = inst.sling_spread;
        const float face_length = inst.sling_face_length;
        const float tilt = inst.sling_tilt_deg * kPi / 180.0f;

        const float bx = inst.pos[0] - spread / 2.0f;
        const float by = inst.pos[1] - 0.035f;
        const float tx = bx + face_length * -std::sin(tilt);
        const float ty = by + face_length * std::cos(tilt);
        const float kx = bx + -0.038f;
        const float ky = by + 0.015f;
        const float mx = 2.0f * inst.pos[0];
        auto mir = [mx](float x) { return mx - x; };

        out.push_back(
            Element{make_wall(pid + "_left_wall",
                              inst.layer,
                              inst.tags,
                              {point(bx, by), point(tx, ty), point(kx, ky), point(bx, by)},
                              MaterialId::Rubber,
                              true)});
        out.push_back(Element{make_wall(
            pid + "_right_wall",
            inst.layer,
            inst.tags,
            {point(mir(bx), by), point(mir(tx), ty), point(mir(kx), ky), point(mir(bx), by)},
            MaterialId::Rubber,
            true)});

        SlingshotDef left_sling;
        left_sling.id = pid + "_left_sling";
        left_sling.layer = inst.layer;
        left_sling.tags = inst.tags;
        left_sling.face_a[0] = tx;
        left_sling.face_a[1] = ty;
        left_sling.face_b[0] = bx;
        left_sling.face_b[1] = by;
        left_sling.kick_speed = inst.sling_kick_speed;
        left_sling.cooldown_ms = 80.0f;
        out.push_back(Element{std::move(left_sling)});

        SlingshotDef right_sling;
        right_sling.id = pid + "_right_sling";
        right_sling.layer = inst.layer;
        right_sling.tags = inst.tags;
        right_sling.face_a[0] = mir(tx);
        right_sling.face_a[1] = ty;
        right_sling.face_b[0] = mir(bx);
        right_sling.face_b[1] = by;
        right_sling.kick_speed = inst.sling_kick_speed;
        right_sling.cooldown_ms = 80.0f;
        out.push_back(Element{std::move(right_sling)});
        return out;
    }

    if (inst.prefab == "inlane_outlane_pair") {
        // §5.4: one side's inlane/outlane assembly (7 children).
        // Authored coordinates are the left-side layout; the right side
        // mirrors about x = mirror_axis_x (§5.4).
        auto mirror = [&inst](float x) {
            return inst.right_side ? 2.0f * inst.mirror_axis_x - x : x;
        };

        std::vector<PathNode> side_path = {point(mirror(0.000f), 0.360f),
                                           point(mirror(0.096f), 0.058f)};
        out.push_back(Element{make_wall(
            pid + "_side_wall", inst.layer, inst.tags, std::move(side_path), MaterialId::Wood)});

        std::vector<PathNode> div_path = {point(mirror(0.062f), 0.268f),
                                          point(mirror(0.122f), 0.138f)};
        out.push_back(Element{make_wall(
            pid + "_divider", inst.layer, inst.tags, std::move(div_path), MaterialId::Wood)});

        PostDef post;
        post.id = pid + "_top_post";
        post.layer = inst.layer;
        post.tags = inst.tags;
        post.pos[0] = mirror(0.062f);
        post.pos[1] = 0.268f;
        post.radius = 0.008f;
        post.material = MaterialId::Rubber;
        out.push_back(Element{std::move(post)});

        RolloverDef outlane;
        outlane.id = pid + "_outlane_rollover";
        outlane.layer = inst.layer;
        outlane.pos[0] = mirror(0.045f);
        outlane.pos[1] = 0.240f;
        outlane.facing_deg = inst.right_side ? 180.0f - (-72.0f) : -72.0f;
        out.push_back(Element{std::move(outlane)});

        RolloverDef inlane;
        inlane.id = pid + "_inlane_rollover";
        inlane.layer = inst.layer;
        inlane.pos[0] = mirror(0.105f);
        inlane.pos[1] = 0.185f;
        inlane.facing_deg = inst.right_side ? 180.0f - (-65.0f) : -65.0f;
        out.push_back(Element{std::move(inlane)});

        LightDef out_light;
        out_light.id = pid + "_outlane_light";
        out_light.layer = inst.layer;
        out_light.tags = inst.tags;
        out_light.pos[0] = mirror(0.052f);
        out_light.pos[1] = 0.205f;
        out_light.color = "insert_primary";
        out.push_back(Element{std::move(out_light)});

        LightDef in_light;
        in_light.id = pid + "_inlane_light";
        in_light.layer = inst.layer;
        in_light.tags = inst.tags;
        in_light.pos[0] = mirror(0.115f);
        in_light.pos[1] = 0.155f;
        in_light.color = "insert_primary";
        out.push_back(Element{std::move(in_light)});
        return out;
    }

    if (inst.prefab == "orbit") {
        // §5.5: guide wall inset mouth_x from the boundary + two entry
        // switches (open gates spanning the lane).
        const float w = partial.width;
        const float h = partial.height;
        const float r = inst.top_radius;
        const float d = inst.mouth_x;

        WallDef guide;
        guide.id = pid + "_guide_wall";
        guide.layer = inst.layer;
        guide.tags = inst.tags;
        guide.material = MaterialId::Wood;
        guide.path = {
            point(d, inst.entry_y_left),
            point(d, h - r),
            arc(r, h - d, r - d, true),
            point(w - r, h - d),
            arc(w - d, h - r, r - d, true),
            point(w - d, inst.entry_y_right),
        };
        out.push_back(Element{std::move(guide)});

        GateDef left_switch;
        left_switch.id = pid + "_left_switch";
        left_switch.layer = inst.layer;
        left_switch.pos[0] = d / 2.0f;
        left_switch.pos[1] = inst.entry_y_left + 0.05f;
        left_switch.width = inst.mouth_x;
        left_switch.facing_deg = 90.0f;
        left_switch.state_open = true; // open sensor gate (§5.5)
        out.push_back(Element{std::move(left_switch)});

        GateDef right_switch;
        right_switch.id = pid + "_right_switch";
        right_switch.layer = inst.layer;
        right_switch.pos[0] = w - d / 2.0f;
        right_switch.pos[1] = inst.entry_y_right + 0.02f;
        right_switch.width = inst.mouth_x;
        right_switch.facing_deg = 90.0f;
        right_switch.state_open = true; // open sensor gate (§5.5)
        out.push_back(Element{std::move(right_switch)});
        return out;
    }

    throw TableLoadError("unknown prefab '" + inst.prefab + "'", "/prefabs", "");
}

namespace {

void validate(const TableDef& def, const std::filesystem::path& file) {
    // §2 hard ranges.
    if (def.width < 0.40f || def.width > 0.70f || def.height < 0.80f || def.height > 1.40f) {
        fail("playfield.size out of range [0.40..0.70] x [0.80..1.40]", "/playfield/size", file);
    }
    if (def.slope_deg < 4.0f || def.slope_deg > 8.0f) {
        fail("slope_deg out of range [4.0, 8.0]", "/playfield/slope_deg", file);
    }
    if (def.ball_count < 1 || def.ball_count > 6) {
        fail("ball_count out of range [1, 6]", "/playfield/ball_count", file);
    }

    bool has_closed_wall = false; // V003
    int plunger_count = 0;        // V023
    int trough_count = 0;         // V023
    int outhole_count = 0;        // V008
    int trough_capacity = 0;

    for (const Element& e : def.elements) {
        if (std::holds_alternative<WallDef>(e.def)) {
            const WallDef& w = std::get<WallDef>(e.def);
            has_closed_wall = has_closed_wall || w.closed;
            for (const PathNode& n : w.path) {
                const float x = n.is_arc ? n.arc.to[0] : n.point[0];
                const float y = n.is_arc ? n.arc.to[1] : n.point[1];
                if (x < 0.0f || x > def.width || y < 0.0f || y > def.height) {
                    fail("wall '" + w.id + "' has a node outside the playfield",
                         "/elements/" + e.id(),
                         file);
                }
            }
        } else if (std::holds_alternative<PlungerDef>(e.def)) {
            ++plunger_count;
        } else if (std::holds_alternative<TroughDef>(e.def)) {
            ++trough_count;
            trough_capacity = std::get<TroughDef>(e.def).capacity;
        } else if (std::holds_alternative<OutholeDef>(e.def)) {
            ++outhole_count;
        }
    }
    if (!has_closed_wall) {
        fail("no closed wall: the outer boundary is required (V003)", "/elements", file);
    }
    if (plunger_count > 1) {
        fail("more than one plunger (V023)", "/elements", file);
    }
    if (trough_count > 1) {
        fail("more than one trough (V023)", "/elements", file);
    }
    if (outhole_count < 1) {
        fail("no outhole: at least one drain sensor is required (V008)", "/elements", file);
    }
    if (trough_count == 1 && trough_capacity < def.ball_count) {
        fail("trough capacity < playfield.ball_count (V009)", "/elements", file);
    }
}

} // namespace

TableDef load_table(const std::filesystem::path& table_dir) {
    const std::filesystem::path file = table_dir / "table.json";
    std::ifstream in(file);
    if (!in) {
        throw TableLoadError("cannot open table.json", "", file);
    }

    json doc;
    try {
        doc = json::parse(in, nullptr, true, true); // comments allowed (canon §5.5)
    } catch (const json::parse_error& e) {
        throw TableLoadError(std::string("JSON parse error: ") + e.what(), "", file);
    }
    if (!doc.is_object()) {
        fail("table.json must be an object", "", file);
    }

    TableDef def;

    if (!doc.contains("format_version") || doc.at("format_version") != 1) {
        fail("format_version must be 1 (V022)", "/format_version", file);
    }
    if (!doc.contains("meta") || !doc.at("meta").is_object()) {
        fail("missing required object 'meta'", "/meta", file);
    }
    const json& meta = doc.at("meta");
    def.slug = get_string(meta, "slug", "", "/meta", file);
    def.name = get_string(meta, "name", "", "/meta", file);
    def.theme = get_string(meta, "theme", "", "/meta", file);
    def.author = get_string(meta, "author", "", "/meta", file);
    def.description = get_string(meta, "description", "", "/meta", file);
    def.rules_card = get_string(meta, "rules_card", "", "/meta", file);
    for (const char* key : {"slug", "name", "theme", "author", "description", "rules_card"}) {
        const std::string v = meta.value(key, std::string());
        if (v.empty()) {
            fail(std::string("missing required meta field '") + key + "'",
                 std::string("/meta/") + key,
                 file);
        }
    }

    if (doc.contains("playfield")) {
        const json& pf = doc.at("playfield");
        if (pf.contains("size")) {
            const json& size = pf.at("size");
            if (!size.is_array() || size.size() != 2 || !size[0].is_number() ||
                !size[1].is_number()) {
                fail("playfield.size must be [w, h] numbers", "/playfield/size", file);
            }
            def.width = size[0].get<float>();
            def.height = size[1].get<float>();
        }
        def.slope_deg = get_number(pf, "slope_deg", def.slope_deg, "/playfield", file);
        def.ball_count =
            int(get_number(pf, "ball_count", float(def.ball_count), "/playfield", file));
        def.layer1_z = get_number(pf, "layer1_z", def.layer1_z, "/playfield", file);
    }

    if (doc.contains("physics")) {
        const json& ph = doc.at("physics");
        def.physics.present = true;
        def.physics.rolling_resistance =
            get_number(ph, "rolling_resistance", def.physics.rolling_resistance, "/physics", file);
        def.physics.restitution_falloff = get_number(
            ph, "restitution_falloff", def.physics.restitution_falloff, "/physics", file);
        def.physics.restitution_soft =
            get_number(ph, "restitution_soft", def.physics.restitution_soft, "/physics", file);
        def.physics.live_catch_window_ms = get_number(
            ph, "live_catch_window_ms", def.physics.live_catch_window_ms, "/physics", file);
        def.physics.live_catch_factor =
            get_number(ph, "live_catch_factor", def.physics.live_catch_factor, "/physics", file);
    }

    if (doc.contains("materials")) {
        const json& mats = doc.at("materials");
        if (!mats.is_object()) {
            fail("materials must be an object", "/materials", file);
        }
        for (auto it = mats.begin(); it != mats.end(); ++it) {
            int idx = -1;
            if (it.key() == "wood") {
                idx = 0;
            } else if (it.key() == "steel") {
                idx = 1;
            } else if (it.key() == "rubber") {
                idx = 2;
            } else if (it.key() == "plastic") {
                idx = 3;
            } else {
                fail("unknown material '" + it.key() + "' (V021)", "/materials", file);
            }
            MaterialOverride& mo = def.materials[idx];
            mo.present = true;
            const std::string ptr = "/materials/" + it.key();
            mo.restitution = get_number(it.value(), "restitution", -1.0f, ptr, file);
            mo.mu_s = get_number(it.value(), "friction_static", -1.0f, ptr, file);
            mo.mu_k = get_number(it.value(), "friction_kinetic", -1.0f, ptr, file);
            mo.spin_transfer = get_number(it.value(), "spin_transfer", -1.0f, ptr, file);
        }
    }

    if (!doc.contains("elements") || !doc.at("elements").is_array()) {
        fail("'elements' array is required", "/elements", file);
    }
    for (size_t i = 0; i < doc.at("elements").size(); ++i) {
        def.elements.push_back(parse_element(doc.at("elements")[i], i, file));
    }

    // Prefab expansion (§5): children append in array order. The §5.2
    // lane/orbit merge decision is order-independent: every orbit's right
    // mouth is measured against every lane's wall top before expansion.
    if (doc.contains("prefabs") && doc.at("prefabs").is_array()) {
        const json& prefabs = doc.at("prefabs");

        std::vector<PrefabParams> params;
        for (size_t i = 0; i < prefabs.size(); ++i) {
            params.push_back(parse_prefab(prefabs[i], i, file));
        }

        for (PrefabParams& lane : params) {
            if (lane.prefab != "plunger_lane") {
                continue;
            }
            const float xw = def.width - lane.lane_width;
            const float y_gate = lane.top_y + 0.008f;
            for (const PrefabParams& orbit : params) {
                if (orbit.prefab != "orbit") {
                    continue;
                }
                const float mx = def.width - orbit.mouth_x;
                const float my = orbit.entry_y_right;
                if (std::sqrt((mx - xw) * (mx - xw) + (my - y_gate) * (my - y_gate)) <= 0.045f) {
                    lane.merged_lane = true;
                    break;
                }
            }
        }

        for (size_t i = 0; i < params.size(); ++i) {
            std::vector<Element> children;
            try {
                children = expand_prefab(def, to_instance(params[i]));
            } catch (TableLoadError& err) {
                err.json_pointer = "/prefabs/" + std::to_string(i);
                err.file = file;
                throw;
            }
            for (Element& child : children) {
                def.elements.push_back(std::move(child));
            }
        }
    }

    // V001: unique ids across everything.
    for (size_t i = 0; i < def.elements.size(); ++i) {
        for (size_t j = i + 1; j < def.elements.size(); ++j) {
            if (def.elements[i].id() == def.elements[j].id()) {
                fail("duplicate element id '" + def.elements[i].id() + "' (V001)",
                     "/elements",
                     file);
            }
        }
    }

    validate(def, file);
    return def;
}

} // namespace tb::table
