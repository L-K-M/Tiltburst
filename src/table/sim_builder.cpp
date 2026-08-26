#include "table/sim_builder.h"

#include "sim/elements.h"
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
// endpoint. Consecutive coincident points collapse (a closed path's last
// node equal to the first adds one cap, not two).
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
        c.a1 = a1; // CCW span from a0 to a1 (may wrap past 0; tests wrap)
        c.element_id = element_id;
        c.sub_index = next_sub++;
        c.layer = uint8_t(w.layer);
        c.material = mat;
        out.colliders.push_back(c);
    };

    // Walk nodes; track the current position for arcs.
    tb::sim::Vec2 cur{w.path[0].point[0], w.path[0].point[1]};
    tb::sim::Vec2 last_point_added = cur;
    add_point(cur);
    for (size_t i = 1; i < w.path.size(); ++i) {
        const PathNode& node = w.path[i];
        if (!node.is_arc) {
            const tb::sim::Vec2 next{node.point[0], node.point[1]};
            add_segment(cur, next);
            if (std::abs(next.x - last_point_added.x) > 1e-6f ||
                std::abs(next.y - last_point_added.y) > 1e-6f) {
                add_point(next);
                last_point_added = next;
            }
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
        // The ±h center choice in §3.1 selects the minor arc by
        // construction; no clamping needed.
        float a_start = std::atan2(cur.y - center.y, cur.x - center.x);
        float a_end = std::atan2(to.y - center.y, to.x - center.x);
        const float span = wrap_ccw_local(a_end - a_start);
        if (node.arc.cw) {
            // CW travel from cur to to: stored CCW span runs end→start.
            add_arc(center, node.arc.radius, a_end, wrap_ccw_local(a_end - span));
        } else {
            add_arc(center, node.arc.radius, a_start, wrap_ccw_local(a_start + span));
        }
        if (std::abs(to.x - last_point_added.x) > 1e-6f ||
            std::abs(to.y - last_point_added.y) > 1e-6f) {
            add_point(to);
            last_point_added = to;
        }
        cur = to;
    }
    if (w.closed) {
        const tb::sim::Vec2 first{w.path[0].point[0], w.path[0].point[1]};
        if (cur.x != first.x || cur.y != first.y) {
            add_segment(cur, first); // closing segment; first node cap exists
        }
    }
}

// Unit vector along facing_deg (0° = +x, 90° = +y).
tb::sim::Vec2 facing_vec(float facing_deg) {
    const float phi = facing_deg * kPi / 180.0f;
    return {std::cos(phi), std::sin(phi)};
}

} // namespace

void build_sim(const TableDef& def, tb::sim::SimState& out) {
    // Reset in place (SimState is non-copyable: atomic rings). The runtime
    // timeline (tick, rings, RNG streams) survives a hot-reload rebuild.
    out.colliders.clear();
    out.flippers.clear();
    out.outholes.clear();
    out.lights.clear();
    out.slingshots.clear();
    out.pop_bumpers.clear();
    out.standups.clear();
    out.rollovers.clear();
    out.gates.clear();
    out.spinners.clear();
    out.kickers.clear();
    out.drop_banks.clear();
    out.captives.clear();
    out.ball_locks.clear();
    out.ball_count = def.ball_count;
    out.locked_balls = 0;
    out.ball_save = {};
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
    out.live_catch_window_ticks =
        def.physics.present ? def.physics.live_catch_window_ms : tb::sim::kLiveCatchWindowTicks;
    out.live_catch_factor =
        def.physics.present ? def.physics.live_catch_factor : tb::sim::kLiveCatchFactor;

    // Material overrides (§2 materials) over the canonical rows.
    {
        for (size_t i = 0; i < std::size(def.materials); ++i) {
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

        if (std::holds_alternative<WallDef>(e.def)) {
            bake_wall(std::get<WallDef>(e.def), element_id, out, next_sub);
        } else if (std::holds_alternative<PostDef>(e.def)) {
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
            if (f.input == "left") {
                sim_flipper.params.action = 0;
            } else if (f.input == "right") {
                sim_flipper.params.action = 1;
            } else if (f.input == "upper_left") {
                sim_flipper.params.action = 2;
            } else {
                sim_flipper.params.action = 3; // upper_right (validated at load)
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
        } else if (std::holds_alternative<SlingshotDef>(e.def)) {
            const SlingshotDef& d = std::get<SlingshotDef>(e.def);
            // Face segment collider (rubber) — endpoints capped.
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Segment;
            c.a = {d.face_a[0], d.face_a[1]};
            c.b = {d.face_b[0], d.face_b[1]};
            c.element_id = element_id;
            c.sub_index = next_sub++;
            c.layer = uint8_t(d.layer);
            c.material = tb::sim::MaterialId::Rubber;
            out.colliders.push_back(c);

            tb::sim::SlingshotElem sl;
            sl.common.kind = tb::sim::ElementKind::Slingshot;
            sl.common.table_id = element_id;
            sl.common.layer = uint8_t(d.layer);
            sl.common.cooldown_ticks = uint32_t(d.cooldown_ms);
            sl.face_a = c.a;
            sl.face_b = c.b;
            // Active normal: left of a→b (§6.2); authors order the face so
            // it points into the playfield.
            tb::sim::Vec2 dseg = c.b - c.a;
            const float dlen = std::sqrt(dseg.x * dseg.x + dseg.y * dseg.y);
            sl.face_normal = dlen > 1e-9f ? tb::sim::Vec2{-dseg.y / dlen, dseg.x / dlen}
                                          : tb::sim::Vec2{0.0f, 1.0f}; // loader rejects these
            sl.kick_speed = d.kick_speed;
            out.slingshots.push_back(sl);
        } else if (std::holds_alternative<PopBumperDef>(e.def)) {
            const PopBumperDef& d = std::get<PopBumperDef>(e.def);
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Point;
            c.a = {d.pos[0], d.pos[1]};
            c.radius = d.radius;
            c.element_id = element_id;
            c.sub_index = next_sub++;
            c.layer = uint8_t(d.layer);
            c.material = tb::sim::MaterialId::Rubber;
            out.colliders.push_back(c);

            tb::sim::PopBumperElem pop;
            pop.common.kind = tb::sim::ElementKind::PopBumper;
            pop.common.table_id = element_id;
            pop.common.layer = uint8_t(d.layer);
            pop.common.cooldown_ticks = uint32_t(d.cooldown_ms);
            pop.pos = c.a;
            pop.radius = d.radius;
            pop.kick_speed = d.kick_speed;
            out.pop_bumpers.push_back(pop);
        } else if (std::holds_alternative<StandupTargetDef>(e.def)) {
            const StandupTargetDef& d = std::get<StandupTargetDef>(e.def);
            tb::sim::Vec2 f = facing_vec(d.facing_deg);
            tb::sim::Vec2 half{-f.y * d.width * 0.5f, f.x * d.width * 0.5f};
            tb::sim::Collider c{};
            c.kind = tb::sim::Collider::Kind::Segment;
            c.a = {d.pos[0] - half.x, d.pos[1] - half.y};
            c.b = {d.pos[0] + half.x, d.pos[1] + half.y};
            c.element_id = element_id;
            c.sub_index = next_sub++;
            c.layer = uint8_t(d.layer);
            c.material = tb::sim::MaterialId::Plastic;
            out.colliders.push_back(c);

            tb::sim::StandupTargetElem st;
            st.common.kind = tb::sim::ElementKind::StandupTarget;
            st.common.table_id = element_id;
            st.common.layer = uint8_t(d.layer);
            st.common.cooldown_ticks = 100; // §6.4 debounce
            st.face_a = c.a;
            st.face_b = c.b;
            st.face_normal = f;
            st.min_speed = d.min_speed;
            out.standups.push_back(st);
        } else if (std::holds_alternative<RolloverDef>(e.def)) {
            const RolloverDef& d = std::get<RolloverDef>(e.def);
            tb::sim::RolloverElem ro;
            ro.common.kind = tb::sim::ElementKind::Rollover;
            ro.common.table_id = element_id;
            ro.common.layer = uint8_t(d.layer);
            const tb::sim::Vec2 f = facing_vec(d.facing_deg);
            ro.a = {d.pos[0] - f.x * 0.025f, d.pos[1] - f.y * 0.025f};
            ro.b = {d.pos[0] + f.x * 0.025f, d.pos[1] + f.y * 0.025f};
            ro.armed = true; // NSDMI covers it; explicit at the bake site
            out.rollovers.push_back(ro);
        } else if (std::holds_alternative<GateDef>(e.def)) {
            const GateDef& d = std::get<GateDef>(e.def);
            tb::sim::GateElem g;
            g.common.kind = tb::sim::ElementKind::Gate;
            g.common.table_id = element_id;
            g.common.layer = uint8_t(d.layer);
            const tb::sim::Vec2 f = facing_vec(d.facing_deg);
            const tb::sim::Vec2 half{-f.y * d.width * 0.5f, f.x * d.width * 0.5f};
            g.a = {d.pos[0] - half.x, d.pos[1] - half.y};
            g.b = {d.pos[0] + half.x, d.pos[1] + half.y};
            g.face_normal = f;
            g.state = d.state_closed ? tb::sim::GateState::Closed
                      : d.state_open ? tb::sim::GateState::Open
                                     : tb::sim::GateState::OneWay;
            g.mechanical = !d.state_open && !d.state_closed;
            out.gates.push_back(g);
        } else if (std::holds_alternative<SpinnerDef>(e.def)) {
            const SpinnerDef& d = std::get<SpinnerDef>(e.def);
            tb::sim::SpinnerElem sp;
            sp.common.kind = tb::sim::ElementKind::Spinner;
            sp.common.table_id = element_id;
            sp.common.layer = uint8_t(d.layer);
            const tb::sim::Vec2 f = facing_vec(d.facing_deg);
            // Trigger segment ⊥ facing_deg, length 0.025 (§6.6).
            const tb::sim::Vec2 half{-f.y * 0.0125f, f.x * 0.0125f};
            sp.a = {d.pos[0] + half.x, d.pos[1] + half.y};
            sp.b = {d.pos[0] - half.x, d.pos[1] - half.y};
            sp.face_normal = f;
            out.spinners.push_back(sp);
        }
        // M7+ types: parsed and retained in the TableDef; their sims arrive
        // with their milestones.
    }

    out.grid.build(out.colliders, out.width, out.height);
}

} // namespace tb::table
