#include "table/sim_builder.h"

#include "sim/flipper.h"

#include <cmath>

namespace tb::table {

namespace {

constexpr float kPi = 3.14159265358979f;

tb::sim::MaterialId to_sim_material(MaterialId m) {
    return static_cast<tb::sim::MaterialId>(static_cast<uint8_t>(m));
}

float wrap_ccw_local(float a) {
    const float two_pi = 2.0f * kPi;
    while (a < 0.0f) {
        a += two_pi;
    }
    while (a >= two_pi) {
        a -= two_pi;
    }
    return a;
}

// Appends one wall path's colliders (08-physics.md §3.1): segments and
// arcs, plus a point collider at every node (corner caps) and arc
// endpoint, deduplicated within the path.
void bake_wall(const WallDef& w, uint16_t element_id, tb::sim::SimState& out, uint16_t& next_sub) {
    const tb::sim::MaterialId mat = to_sim_material(w.material);
    const size_t n_nodes = w.path.size() + (w.closed ? 1 : 0);

    auto add_segment = [&](tb::sim::Vec2 a, tb::sim::Vec2 b) {
        tb::sim::Collider c{};
        c.kind = tb::sim::Collider::Kind::Segment;
        c.a = a;
        c.b = b;
        c.element_id = element_id;
        c.sub_index = next_sub++;
        c.layer = uint8_t(w.layer);
        c.material = mat;
        out.colliders.push_back(c);
    };
    auto add_point = [&](tb::sim::Vec2 p) {
        tb::sim::Collider c{};
        c.kind = tb::sim::Collider::Kind::Point;
        c.a = p;
        c.radius = 0.0f; // bare corner cap
        c.element_id = element_id;
        c.sub_index = next_sub++;
        c.layer = uint8_t(w.layer);
        c.material = mat;
        out.colliders.push_back(c);
    };
    auto add_arc = [&](tb::sim::Vec2 center, float radius, float a0, float a1) {
        tb::sim::Collider c{};
        c.kind = tb::sim::Collider::Kind::Arc;
        c.a = center;
        c.radius = radius;
        c.a0 = a0;
        c.a1 = a1; // stored CCW span (a1 > a0)
        c.element_id = element_id;
        c.sub_index = next_sub++;
        c.layer = uint8_t(w.layer);
        c.material = mat;
        out.colliders.push_back(c);
    };

    // Walk nodes; track the current position for arcs.
    tb::sim::Vec2 cur{w.path[0].point[0], w.path[0].point[1]};
    add_point(cur);
    for (size_t i = 1; i < w.path.size(); ++i) {
        const PathNode& node = w.path[i];
        if (!node.is_arc) {
            const tb::sim::Vec2 next{node.point[0], node.point[1]};
            add_segment(cur, next);
            add_point(next);
            cur = next;
            continue;
        }

        // §3.1 arc baking: C = M ± h·perp(d̂), + for ccw, − for cw.
        const tb::sim::Vec2 to{node.arc.to[0], node.arc.to[1]};
        const tb::sim::Vec2 d = to - cur;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const float h =
            std::sqrt(std::max(0.0f, node.arc.radius * node.arc.radius - len * len * 0.25f));
        const tb::sim::Vec2 m{(cur.x + to.x) * 0.5f, (cur.y + to.y) * 0.5f};
        const tb::sim::Vec2 dhat{d.x / len, d.y / len};
        const tb::sim::Vec2 perp{-dhat.y, dhat.x};
        const float sign = node.arc.cw ? -1.0f : 1.0f;
        const tb::sim::Vec2 center{m.x + sign * h * perp.x, m.y + sign * h * perp.y};

        // Start/end angles around the center; store CCW from start to end.
        float a_start = std::atan2(cur.y - center.y, cur.x - center.x);
        float a_end = std::atan2(to.y - center.y, to.x - center.x);
        float span = wrap_ccw_local(a_end - a_start);
        if (span > kPi) {
            // Minor arc only (§3): the other way around.
            span -= 2.0f * kPi;
        }
        if (node.arc.cw) {
            // CW travel from cur to to: stored CCW span runs end→start.
            add_arc(center, node.arc.radius, a_end, wrap_ccw_local(a_end - span));
        } else {
            add_arc(center, node.arc.radius, a_start, wrap_ccw_local(a_start + span));
        }
        add_point(to);
        cur = to;
    }
    if (w.closed) {
        const tb::sim::Vec2 first{w.path[0].point[0], w.path[0].point[1]};
        if (cur.x != first.x || cur.y != first.y) {
            add_segment(cur, first); // closing segment; first node cap exists
        }
    }
}

} // namespace

void build_sim(const TableDef& def, tb::sim::SimState& out) {
    // Reset in place (SimState is non-copyable: atomic rings). The runtime
    // timeline (tick, rings, RNG streams) survives a hot-reload rebuild.
    out.colliders.clear();
    out.flippers.clear();
    out.outholes.clear();
    out.lights.clear();
    for (auto& b : out.balls) {
        b = tb::sim::Ball{};
    }
    out.has_plunger = false;
    out.plunger = {};
    out.trough_balls = 0;
    out.serve_delay_ticks = 0;
    out.stats = {};
    {
        using MI = tb::sim::MaterialId;
        out.mats[uint8_t(MI::Wood)] = tb::sim::material_row(MI::Wood);
        out.mats[uint8_t(MI::Steel)] = tb::sim::material_row(MI::Steel);
        out.mats[uint8_t(MI::Rubber)] = tb::sim::material_row(MI::Rubber);
        out.mats[uint8_t(MI::Plastic)] = tb::sim::material_row(MI::Plastic);
        out.mats[uint8_t(MI::FlipperRubber)] = tb::sim::material_row(MI::FlipperRubber);
    }

    out.width = def.width;
    out.height = def.height;
    out.slope_deg = def.slope_deg;
    out.mu_rr = def.physics.present ? def.physics.rolling_resistance : tb::sim::kRollMu;
    out.restitution_falloff = def.physics.present ? def.physics.restitution_falloff : 0.12f;
    out.restitution_soft = def.physics.present ? def.physics.restitution_soft : 0.5f;
    if (def.physics.present) {
        out.live_catch_window_ticks = def.physics.live_catch_window_ms; // ms → ticks (1 ms)
        out.live_catch_factor = def.physics.live_catch_factor;
    }

    // Material overrides (§2 materials) over the canonical rows.
    {
        for (int i = 0; i < 4; ++i) {
            const MaterialOverride& mo = def.materials[i];
            if (!mo.present) {
                continue;
            }
            tb::sim::Material& m = out.mats[uint8_t(i)];
            if (mo.restitution >= 0.0f) {
                m.restitution = mo.restitution;
            }
            if (mo.mu_s >= 0.0f) {
                m.mu_s = mo.mu_s;
            }
            if (mo.mu_k >= 0.0f) {
                m.mu_k = mo.mu_k;
            }
            if (mo.spin_transfer >= 0.0f) {
                m.spin_transfer = mo.spin_transfer;
            }
        }
    }

    uint16_t next_sub = 0;

    for (size_t idx = 0; idx < def.elements.size(); ++idx) {
        const Element& e = def.elements[idx];
        const uint16_t element_id = uint16_t(idx);
        const std::string& type = e.id(); // silence unused-warning pattern

        if (e.type_name() == std::string("wall")) {
            bake_wall(std::get<WallDef>(e.def), element_id, out, next_sub);
        } else if (e.type_name() == std::string("post")) {
            const PostDef& p = std::get<PostDef>(e.def);
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Point;
            c.a = {p.pos[0], p.pos[1]};
            c.radius = p.radius;
            c.element_id = element_id;
            c.sub_index = next_sub++;
            c.layer = uint8_t(p.layer);
            c.material = to_sim_material(p.material);
            out.colliders.push_back(c);
        } else if (e.type_name() == std::string("flipper")) {
            const FlipperDef& f = std::get<FlipperDef>(e.def);
            tb::sim::Flipper sim_flipper;
            sim_flipper.params.pivot = {f.pos[0], f.pos[1]};
            sim_flipper.params.length = f.length;
            sim_flipper.params.radius_base = f.radius_base;
            sim_flipper.params.radius_tip = f.radius_tip;
            sim_flipper.params.rest_angle_deg = f.rest_angle_deg;
            sim_flipper.params.swing_deg = f.swing_deg;
            sim_flipper.params.side_sign = f.left_side ? +1 : -1;
            sim_flipper.params.strength = f.strength;
            if (f.input == "left" || f.input == "upper_left") {
                sim_flipper.params.action = f.input == "left" ? 0 : 2;
            } else {
                sim_flipper.params.action = f.input == "right" ? 1 : 3;
            }
            sim_flipper.theta = sim_flipper.params.rest_rad();
            sim_flipper.theta_start = sim_flipper.theta;
            out.flippers.push_back(sim_flipper);
        } else if (e.type_name() == std::string("plunger")) {
            const PlungerDef& p = std::get<PlungerDef>(e.def);
            out.has_plunger = true;
            out.plunger.pos = {p.pos[0], p.pos[1]};
            const float phi = p.launch_angle_deg * kPi / 180.0f;
            out.plunger.lane_dir = {std::cos(phi), std::sin(phi)};
            out.plunger.max_speed = p.max_speed;
            out.plunger.charge_ticks = p.charge_time_s * 1000.0f;
            out.plunger.auto_launch = p.auto_launch;
            out.plunger.auto_delay_ticks = uint32_t(p.auto_delay_ms);

            // Plunger face: 0.03 m steel segment perpendicular to ĵ at pos
            // (08 §6.16), so idle balls rest on it.
            const tb::sim::Vec2 perp{-out.plunger.lane_dir.y, out.plunger.lane_dir.x};
            tb::sim::Collider face{};
            face.kind = tb::sim::Collider::Kind::Segment;
            face.a = {p.pos[0] - perp.x * 0.015f, p.pos[1] - perp.y * 0.015f};
            face.b = {p.pos[0] + perp.x * 0.015f, p.pos[1] + perp.y * 0.015f};
            face.element_id = element_id;
            face.sub_index = next_sub++;
            face.layer = 0;
            face.material = tb::sim::MaterialId::Steel;
            out.colliders.push_back(face);
        } else if (e.type_name() == std::string("outhole")) {
            const OutholeDef& o = std::get<OutholeDef>(e.def);
            out.outholes.push_back({{o.a[0], o.a[1]}, {o.b[0], o.b[1]}});
        } else if (e.type_name() == std::string("trough")) {
            const TroughDef& t = std::get<TroughDef>(e.def);
            out.trough_balls = def.ball_count; // machine starts loaded
            (void)t;
        } else if (e.type_name() == std::string("light")) {
            const LightDef& l = std::get<LightDef>(e.def);
            out.lights.push_back({{l.pos[0], l.pos[1]}, l.size, false});
        }
        // M6+ types (gate, rollover, slingshot, pop_bumper,
        // standup_target): parsed and retained in the TableDef; their sims
        // arrive with their milestones (04-milestones.md §M5 scope-out).
        (void)type;
    }

    out.grid.build(out.colliders, out.width, out.height);
}

} // namespace tb::table
