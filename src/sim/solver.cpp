#include "sim/solver.h"

#include <algorithm>
#include <cmath>

namespace tb::sim {

namespace {

constexpr double kPiF = 3.14159265358979;
constexpr uint32_t kSlingArmVisualTicks = 60; // §6.2 kicked visual: 60 ms
constexpr uint32_t kPopFlashTicks = 60;       // §6.3 skirt flash window
// §6.6 plate friction as a per-tick factor: 0.55^(1/1000) — ln(0.55)/1000
// = -5.97837e-4, exp of that. std::pow per spinner per tick is hot-path
// waste for a compile-time constant.
constexpr float kSpinnerDecayPerTick = 0.99940234f;
static_assert(kTickDt > 0.0009f && kTickDt < 0.0011f,
              "kSpinnerDecayPerTick assumes 1 ms ticks; recompute 0.55^kTickDt");

float point_segment_distance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const float t = std::clamp(dot(p - a, ab) / std::max(length_sq(ab), 1e-12f), 0.0f, 1.0f);
    return length(p - (a + ab * t));
}

} // namespace

SimState::SimState() {
    mats[uint8_t(MaterialId::Wood)] = material_row(MaterialId::Wood);
    mats[uint8_t(MaterialId::Steel)] = material_row(MaterialId::Steel);
    mats[uint8_t(MaterialId::Rubber)] = material_row(MaterialId::Rubber);
    mats[uint8_t(MaterialId::Plastic)] = material_row(MaterialId::Plastic);
    mats[uint8_t(MaterialId::FlipperRubber)] = material_row(MaterialId::FlipperRubber);
}

namespace {

inline Vec2 clamp_speed(Vec2 v) {
    const float speed_sq = length_sq(v);
    if (speed_sq > kMaxSpeed * kMaxSpeed) {
        const float s = kMaxSpeed / std::sqrt(speed_sq);
        return v * s;
    }
    return v;
}

// §4.2: velocity-dependent restitution. Two regimes:
// - high speed: e falls off with approach speed (kFalloff past kSoft);
// - low speed: viscoelastic cliff, e ramps linearly to zero at kRestSpeed
//   (ADR-021: a flat e down to the cutoff sustains micro-bounce limit
//   cycles — caught balls rattle instead of settling; real rubber is
//   velocity-weakening at low impact speeds).
inline float restitution_curve(const SimState& s, float e, float approach) {
    if (approach < kRestSpeed) {
        return 0.0f;
    }
    const float soft_scale =
        std::min(1.0f, (approach - kRestSpeed) / (s.restitution_soft - kRestSpeed));
    return e * soft_scale /
           (1.0f + s.restitution_falloff * std::max(0.0f, approach - s.restitution_soft));
}

inline void absorb(SimState& s, uint64_t tick, SimEventType type, uint16_t element) {
    // Event-sequence accumulator (16-testing-ci.md §2.4.1): tick, type, id
    // absorbed as raw bytes in field order, rolling, never reset.
    const auto u8 = [](uint64_t v, int shift) {
        return static_cast<unsigned char>((v >> shift) & 0xFFu);
    };
    const unsigned char bytes[12] = {
        u8(tick, 0),
        u8(tick, 8),
        u8(tick, 16),
        u8(tick, 24),
        u8(tick, 32),
        u8(tick, 40),
        u8(tick, 48),
        u8(tick, 56),
        u8(uint16_t(type), 0),
        u8(uint16_t(type), 8),
        u8(element, 0),
        u8(element, 8),
    };
    s.event_seq_hash = fnv1a64(bytes, sizeof(bytes), s.event_seq_hash);
}

} // namespace

namespace {

// Persistent-contact probe (§3.6 pull-back contract). A ball resting on or
// sliding along a surface within kSkin moves too slowly to cross it inside
// one tick, so swept tests never fire and friction never acts — the ball
// free-falls between rare crossings and rattles at the sweep threshold.
// Probe the discrete separation instead: an approaching ball essentially
// touching a surface is an immediate contact at TOI = t_cur.
constexpr float kPersistMargin = kSkin + 1e-9f; // float-noise guard

struct PersistHit {
    bool hit = false;
    Vec2 normal{};
};

PersistHit persist_point(Vec2 p, Vec2 v, Vec2 c, float radius) {
    const Vec2 d = p - c;
    const float dist = length(d);
    if (dist - radius > kPersistMargin) {
        return {};
    }
    const PersistHit hit{true, dist > 1e-9f ? d * (1.0f / dist) : Vec2{0.0f, 1.0f}};
    return dot(v, hit.normal) < 0.0f ? hit : PersistHit{};
}

PersistHit persist_segment(Vec2 p, Vec2 v, Vec2 a, Vec2 b) {
    const Vec2 d = b - a;
    const float len_sq = length_sq(d);
    if (len_sq <= 1e-12f) {
        return {};
    }
    const float len = std::sqrt(len_sq);
    const Vec2 dh = d * (1.0f / len);
    const Vec2 n_line = perp(dh);

    const Vec2 rel = p - a;
    const float u = dot(rel, dh);
    if (u < 0.0f || u > len) {
        return {}; // outside the span: endpoint caps own these regions
    }
    const float sn = dot(rel, n_line);
    if (std::fabs(sn) > kPersistMargin) {
        return {};
    }
    const PersistHit hit{true, sn >= 0.0f ? n_line : n_line * -1.0f};
    return dot(v, hit.normal) < 0.0f ? hit : PersistHit{};
}

PersistHit persist_arc(Vec2 p, Vec2 v, Vec2 c, float radius, float r, float a0, float a1) {
    const Vec2 d = p - c;
    const float dist = length(d);
    if (dist <= 1e-9f) {
        return {};
    }

    Vec2 n{};
    if (dist > radius) {
        // Outer surface.
        if (dist - (radius + r) > kPersistMargin) {
            return {};
        }
        n = d * (1.0f / dist);
    } else if (radius > r && dist < radius - r) {
        // Inner surface.
        if (radius - r - dist > kPersistMargin) {
            return {};
        }
        n = d * (-1.0f / dist);
    } else {
        return {}; // inside the wall band: push-out territory
    }

    // Angular window, same convention as sweep_circle_vs_arc.
    const float phi = wrap_ccw(std::atan2(d.y, d.x) - a0);
    if (phi > wrap_ccw(a1 - a0)) {
        return {};
    }
    return dot(v, n) < 0.0f ? PersistHit{true, n} : PersistHit{};
}

void emit_element_event(
    SimState& s, SimEventType type, uint16_t element, const Ball& ball, float payload) {
    absorb(s, s.tick, type, element);
    SimEvent ev;
    ev.tick = s.tick;
    ev.type = uint16_t(type);
    ev.element = element;
    ev.x = ball.pos.x;
    ev.y = ball.pos.y;
    ev.a = payload;
    ev.data = ball.index;
    s.render_ring.push(s.tick, ev);
    s.game_ring.push(s.tick, ev);
}

} // namespace

float Solver::resolve_surface(SimState& s,
                              Ball& ball,
                              Vec2 n,
                              const Material& mat,
                              Vec2 surface_vel,
                              float live_catch_scale) {
    // §4.1 impulse equations against an infinite-mass moving surface.
    // V_s is zero for static colliders; §3.5 for flippers.
    const Vec2 t = perp(n);

    // Relative contact velocity: u = v_p − V_s.
    const Vec2 vp = ball.vel - kBallRadius * ball.omega_z * perp(n);
    const Vec2 u_vec = vp - surface_vel;
    const float u_n = dot(u_vec, n);
    if (u_n >= 0.0f) {
        return 0.0f; // separating — never apply impulses
    }
    const float u_t = dot(u_vec, t);

    const float approach = -u_n;
    float e_eff = restitution_curve(s, mat.restitution, approach) * live_catch_scale;
    float mu_s = mat.mu_s;
    float mu_k = mat.mu_k;
    if (live_catch_scale < 1.0f) {
        mu_s = std::min(1.0f, 2.0f * mu_s); // tangential grip (§5.4)
        mu_k = std::min(1.0f, 2.0f * mu_k);
    }
    const float j_n = -(1.0f + e_eff) * u_n * kBallMass;

    // Tangential effective mass: 1/m_t = 7/(2m) → m_t = 2m/7.
    const float j_stick = -(2.0f * kBallMass / 7.0f) * u_t;
    float j_t;
    if (std::fabs(j_stick) <= mu_s * j_n) {
        j_t = j_stick;
    } else {
        j_t = -std::copysign(mu_k * j_n, u_t);
    }

    ball.vel += (j_n * n + j_t * t) * (1.0f / kBallMass);
    ball.omega_z += mat.spin_transfer * (-kBallRadius * j_t) / kBallInertia;
    ball.vel = clamp_speed(ball.vel);
    ball.omega_z = std::clamp(ball.omega_z, -kMaxSpin, kMaxSpin);
    return approach;
}

void Solver::resolve_pair(SimState& s, Ball& a, Ball& b, Vec2 n) {
    // §8: equal masses; n̂ points toward ball a.
    const Vec2 t = perp(n);
    const Vec2 u =
        (a.vel - kBallRadius * a.omega_z * perp(n)) - (b.vel + kBallRadius * b.omega_z * perp(n));
    const float u_n = dot(u, n);
    if (u_n >= 0.0f) {
        return;
    }
    const float u_t = dot(u, t);

    const float e_eff = restitution_curve(s, 0.93f, -u_n);
    const float j_n = -(1.0f + e_eff) * u_n * (kBallMass / 2.0f);

    const float j_t = std::clamp(-(kBallMass / 7.0f) * u_t, -0.05f * j_n, 0.05f * j_n);

    const Vec2 impulse = (j_n * n + j_t * t) * (1.0f / kBallMass);
    a.vel += impulse;
    b.vel -= impulse;

    constexpr float kappa = 0.20f;
    const float d_omega = kappa * (-kBallRadius * j_t) / kBallInertia;
    a.omega_z += d_omega;
    b.omega_z += d_omega;

    a.vel = clamp_speed(a.vel);
    b.vel = clamp_speed(b.vel);
    a.omega_z = std::clamp(a.omega_z, -kMaxSpin, kMaxSpin);
    b.omega_z = std::clamp(b.omega_z, -kMaxSpin, kMaxSpin);
}

void Solver::resolve_flipper(SimState& s, Ball& ball, Flipper& f, const FlipperHit& hit) {
    const Material mat = material_row(MaterialId::FlipperRubber);

    // Live catch damping (§5.4): a just-raised HOLD absorbs the ball.
    float scale = 1.0f;
    if (f.state == FlipperState::Hold && f.ticks_since_eos <= kLiveCatchWindowTicks) {
        scale = kLiveCatchFactor;
    }

    resolve_surface(s, ball, hit.normal, mat, hit.surface_vel, scale);
}

Solver::Contact Solver::find_earliest(SimState& s, float t_cur) {
    Contact best;
    best.kind = Contact::None;

    auto consider_static = [&](float toi, uint8_t ball_idx, Vec2 normal, const Collider* col) {
        bool better;
        if (best.kind == Contact::None) {
            better = true;
        } else if (best.kind == Contact::Pair) {
            better = toi <= best.toi + kToiEps; // STATIC before PAIR on ties
        } else {
            better = toi < best.toi - kToiEps ||
                     (toi <= best.toi + kToiEps && (col->element_id < best.collider->element_id ||
                                                    (col->element_id == best.collider->element_id &&
                                                     col->sub_index < best.collider->sub_index)));
        }
        if (!better) {
            return;
        }
        best = Contact{toi, Contact::Static, ball_idx, ball_idx, normal, col};
    };

    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Free || frozen_[bi]) {
            continue;
        }

        const float window = kTickDt - t_cur;
        const Vec2 p_end = ball.pos + ball.vel * window;
        s.grid.query(ball.pos, p_end, kBallRadius, ball.layer, candidates_);

        for (uint32_t ci : candidates_) {
            const Collider& c = s.colliders[ci];
            SweepHit hit;
            bool hit_now = false;

            // Persistent contact first: an approaching ball within kSkin of
            // this surface resolves now, not at a future crossing.
            PersistHit ph;
            switch (c.kind) {
            case Collider::Kind::Point:
                ph = persist_point(ball.pos, ball.vel, c.a, c.radius);
                break;
            case Collider::Kind::Segment:
                ph = persist_segment(ball.pos, ball.vel, c.a, c.b);
                break;
            case Collider::Kind::Arc:
                ph = persist_arc(ball.pos, ball.vel, c.a, c.radius, kBallRadius, c.a0, c.a1);
                break;
            }
            if (ph.hit) {
                consider_static(t_cur, bi, ph.normal, &c);
                continue;
            }

            switch (c.kind) {
            case Collider::Kind::Segment:
                hit_now =
                    sweep_circle_vs_segment(ball.pos, ball.vel, kBallRadius, c.a, c.b, window, hit);
                break;
            case Collider::Kind::Point:
                hit_now = sweep_circle_vs_point(
                    ball.pos, ball.vel, kBallRadius, c.a, c.radius + kBallRadius, window, hit);
                break;
            case Collider::Kind::Arc:
                hit_now = sweep_circle_vs_arc(
                    ball.pos, ball.vel, kBallRadius, c.a, c.radius, c.a0, c.a1, window, hit);
                break;
            }
            if (hit_now) {
                consider_static(t_cur + hit.toi, bi, hit.normal, &c);
            }
        }

        // Dynamic colliders bypass the grid (§3.7): flippers in id order.
        for (uint8_t fi = 0; fi < s.flippers.size(); ++fi) {
            Flipper& f = s.flippers[fi];
            if (ball.layer != 0) {
                continue; // flippers live on layer 0 in v1
            }
            // Persistent flipper contact at this tick's angle.
            const FlipperSep sep =
                flipper_separation(ball.pos, kBallRadius, f, f.theta_start + f.omega * t_cur);
            if (sep.sep <= kPersistMargin && dot(ball.vel - sep.surface_vel, sep.normal) < 0.0f) {
                float toi = t_cur;
                bool better = best.kind == Contact::None || toi < best.toi - kToiEps;
                if (!better && best.kind == Contact::Static && toi <= best.toi + kToiEps) {
                    better = true;
                }
                if (better) {
                    best.toi = toi;
                    best.kind = Contact::Flipper;
                    best.ball = bi;
                    best.ball2 = bi;
                    best.normal = sep.normal;
                    best.collider = nullptr;
                    best.flipper = &f;
                }
                continue;
            }

            FlipperHit fh;
            if (sweep_circle_vs_flipper(ball.pos, ball.vel, kBallRadius, f, window, fh)) {
                float toi = t_cur + fh.toi;
                bool better = best.kind == Contact::None || toi < best.toi - kToiEps;
                // FLIPPER ties: after STATIC, before PAIR.
                if (!better && best.kind == Contact::Static && toi <= best.toi + kToiEps) {
                    better = true; // deterministic: first flipper in id order
                }
                if (better) {
                    best.toi = toi;
                    best.kind = Contact::Flipper;
                    best.ball = bi;
                    best.ball2 = bi;
                    best.normal = fh.normal;
                    best.collider = nullptr;
                    best.flipper = &f;
                }
            }
        }

        // Blocking gates (§6.7): dynamic colliders, element order.
        for (GateElem& g : s.gates) {
            if (ball.layer != g.common.layer) {
                continue;
            }
            const bool blocks_forward = g.state == GateState::Closed;
            const bool blocks_reverse = g.state != GateState::Open;
            const float vn = dot(ball.vel, g.face_normal);
            const bool blocks = (vn > 0.0f && blocks_forward) || (vn <= 0.0f && blocks_reverse);
            if (!blocks) {
                continue;
            }
            SweepHit hit;
            if (sweep_circle_vs_segment(ball.pos, ball.vel, kBallRadius, g.a, g.b, window, hit)) {
                const float toi = t_cur + hit.toi;
                bool better = best.kind == Contact::None || toi < best.toi - kToiEps;
                if (!better && best.kind == Contact::Static && toi <= best.toi + kToiEps) {
                    better = true;
                }
                if (better) {
                    best.toi = toi;
                    best.kind = Contact::Static;
                    best.ball = bi;
                    best.ball2 = bi;
                    best.normal = hit.normal;
                    // Blocking gates resolve through the static path but
                    // need the gate's absorbent steel (e forced 0.3):
                    // mark via a dedicated pseudo-collider.
                    gate_pseudo_.kind = Collider::Kind::Segment;
                    gate_pseudo_.a = g.a;
                    gate_pseudo_.b = g.b;
                    gate_pseudo_.element_id = g.common.table_id;
                    gate_pseudo_.layer = g.common.layer;
                    gate_pseudo_.material = MaterialId::Steel;
                    best.collider = &gate_pseudo_;
                }
            }
        }

        // Spinner slow-wall (§6.6): below 0.15 m/s pass speed the plate is
        // a steel wall (e 0.3).
        for (SpinnerElem& sp : s.spinners) {
            if (ball.layer != sp.common.layer) {
                continue;
            }
            const float s_pass = std::abs(dot(ball.vel, sp.face_normal));
            if (s_pass >= 0.15f) {
                continue;
            }
            SweepHit hit;
            if (sweep_circle_vs_segment(ball.pos, ball.vel, kBallRadius, sp.a, sp.b, window, hit)) {
                const float toi = t_cur + hit.toi;
                bool better = best.kind == Contact::None || toi < best.toi - kToiEps;
                if (!better && best.kind == Contact::Static && toi <= best.toi + kToiEps) {
                    better = true;
                }
                if (better) {
                    best.toi = toi;
                    best.kind = Contact::Static;
                    best.ball = bi;
                    best.ball2 = bi;
                    best.normal = hit.normal;
                    gate_pseudo_.kind = Collider::Kind::Segment;
                    gate_pseudo_.a = sp.a;
                    gate_pseudo_.b = sp.b;
                    gate_pseudo_.element_id = sp.common.table_id;
                    gate_pseudo_.layer = sp.common.layer;
                    gate_pseudo_.material = MaterialId::Steel;
                    best.collider = &gate_pseudo_;
                }
            }
        }

        // Ball-ball pairs (§8): fixed i<j order, same layer only.
        for (uint8_t bj = uint8_t(bi + 1); bj < kMaxBalls; ++bj) {
            Ball& other = s.balls[bj];
            if (!other.live || other.mode != BallMode::Free || other.layer != ball.layer ||
                frozen_[bj]) {
                continue;
            }
            SweepHit hit;
            if (sweep_circle_vs_circle(ball.pos,
                                       ball.vel,
                                       kBallRadius,
                                       other.pos,
                                       other.vel,
                                       kBallRadius,
                                       window,
                                       hit)) {
                const float toi = t_cur + hit.toi;
                bool better;
                if (best.kind == Contact::None) {
                    better = true;
                } else {
                    // PAIR after STATIC on equal toi; strict toi wins.
                    better = best.kind == Contact::Pair ? toi < best.toi - kToiEps
                                                        : toi < best.toi - kToiEps;
                }
                if (better) {
                    // Normal points toward pair-member "a" (= ball bi).
                    best = Contact{toi, Contact::Pair, bi, bj, hit.normal * -1.0f, nullptr};
                }
            }
        }
    }
    return best;
}

void Solver::step(SimState& s, const TickInput& input_latched) {
    const TickInput* input = &input_latched;
    step_body(s, input);
}

void Solver::step(SimState& s, const TickInput* input) {
    step_body(s, input);
}

void Solver::step_body(SimState& s, const TickInput* input) {
    contact_log_n_ = 0;

    // Step 2 — flipper state update (§5.2), id order; (theta_start,
    // omega) held constant for CCD this tick.
    for (Flipper& f : s.flippers) {
        bool pressed = f.enabled && ((input->buttons >> f.params.action) & 1u) != 0u;
        FlipperSim::tick(f, pressed);
    }

    // Step 3 — forces + velocity update (FREE balls, index order).
    const float slope_rad = s.slope_deg * (float(kPi) / 180.0f);
    const float g_slope = kGravity * std::sin(slope_rad);
    const float rr = s.mu_rr * kGravity * std::cos(slope_rad);

    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Free) {
            continue;
        }

        Vec2 acc{0.0f, -g_slope}; // slope gravity

        const float speed = length(ball.vel);
        if (speed > 0.0f) {
            acc -= ball.vel * (s.air_drag * speed); // quadratic drag

            const float rr_drop = rr * kTickDt;
            if (rr_drop >= speed) {
                ball.vel = {0.0f, 0.0f}; // never reverse velocity
            } else {
                ball.vel += ball.vel * (-rr / speed) * kTickDt;
            }
        }

        ball.vel += acc * kTickDt;
        ball.vel = clamp_speed(ball.vel);
        ball.omega_z *= std::exp(-kSpinDamp * kTickDt);
        ball.omega_z = std::clamp(ball.omega_z, -kMaxSpin, kMaxSpin);
    }

    // Step 4 pre-pass — fallback push-out (§3.8), position-only.
    pushout(s);

    // Step 4 — CCD resolution loop on a shared timeline (§3.6).
    float t_cur = 0.0f;
    for (;;) {
        const Contact best = find_earliest(s, t_cur);
        if (best.kind == Contact::None) {
            for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
                Ball& ball = s.balls[bi];
                if (!ball.live || ball.mode != BallMode::Free || frozen_[bi]) {
                    continue;
                }
                ball.pos += ball.vel * (kTickDt - t_cur);
            }
            break;
        }

        const float t_adv = best.toi - kToiEps;
        const float advance = t_adv - t_cur;

        for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
            Ball& ball = s.balls[bi];
            if (!ball.live || ball.mode != BallMode::Free || frozen_[bi]) {
                continue;
            }
            ball.pos += ball.vel * advance;
        }
        t_cur = t_adv;

        if (best.kind == Contact::Flipper) {
            Ball& ball = s.balls[best.ball];
            const float speed_before = length(ball.vel);
            const float t_back = std::min(kSkin / std::max(speed_before, 1e-6f), t_cur);
            ball.pos -= ball.vel * t_back;

            resolve_flipper(
                s,
                ball,
                *best.flipper,
                FlipperHit{best.toi,
                           best.normal,
                           ball.pos - best.normal * kBallRadius,
                           best.flipper->omega * perp(ball.pos - best.normal * kBallRadius -
                                                      best.flipper->params.pivot)});

            resolved_[best.ball]++;
        } else if (best.kind == Contact::Static) {
            Ball& ball = s.balls[best.ball];
            const float speed = length(ball.vel);
            if (speed > 0.0f) {
                const float t_back = std::min(kSkin / speed, t_cur);
                ball.pos -= ball.vel * t_back; // keep kSkin separation
            }

            const float approach = resolve_surface(
                s, ball, best.normal, s.mats[uint8_t(best.collider->material)], {0.0f, 0.0f}, 1.0f);

            if (contact_log_n_ < kContactLogCap) {
                contact_log_[contact_log_n_++] = {best.ball, best.collider->element_id, approach};
            }

            // Collision event for audio/particles (§4.1): emit at speed.
            // Absorb into the sequence hash first (dispatch order).
            absorb(s, s.tick, SimEventType::Collision, best.collider->element_id);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::Collision);
            ev.element = best.collider->element_id;
            ev.x = ball.pos.x;
            ev.y = ball.pos.y;
            ev.a = approach;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);

            resolved_[best.ball]++;
        } else {
            resolve_pair(s, s.balls[best.ball], s.balls[best.ball2], best.normal);
            resolved_[best.ball]++;
            resolved_[best.ball2]++;
        }

        s.stats.toi_iters++;
        if (resolved_[best.ball] > uint32_t(kMaxToiIter) ||
            (best.kind == Contact::Pair && resolved_[best.ball2] > uint32_t(kMaxToiIter))) {
            frozen_[best.ball] = true; // velocity kept, position stops
            s.stats.frozen++;
        }
    }

    // Step 6 — triggers and regions (§2.1): plunger zone, outholes,
    // trough serve, reactive elements. Positions are final for this tick.
    step_regions(s, input);
    step_elements(s);

    // Step 7c — last_safe_pos for balls that ended penetration-free.
    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Free) {
            continue;
        }
        if (!frozen_[bi]) {
            ball.last_safe_pos = ball.pos;
        }
        frozen_[bi] = false;
        resolved_[bi] = 0;
    }

    ++s.tick;
}

void Solver::step_regions(SimState& s, const TickInput* input) {
    constexpr uint32_t kPlungerActionBit = 4;  // 05 §9.1 action index
    constexpr uint32_t kServeDelayTicks = 500; // drain → serve delay (M5 loop)

    // ---- Plunger (08 §6.16) ----
    if (s.has_plunger) {
        bool in_zone = false;
        uint8_t zone_ball = 0xFF;
        for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
            const Ball& b = s.balls[bi];
            if (b.live && b.mode == BallMode::Free && length(b.pos - s.plunger.pos) < 0.04f) {
                in_zone = true;
                zone_ball = bi;
                break;
            }
        }
        const bool was_in_zone = s.plunger.ball_in_zone;
        s.plunger.ball_in_zone = in_zone;

        const bool held = input != nullptr && ((input->buttons >> kPlungerActionBit) & 1u) != 0u;
        if (held && in_zone) {
            s.plunger.held_ticks++; // q grows only while a ball is in the zone
        }
        if (s.plunger.auto_launch && in_zone && !was_in_zone) {
            s.plunger.auto_timer = 0; // settle timer starts on arrival
        }
        if (s.plunger.auto_launch && in_zone) {
            ++s.plunger.auto_timer;
        }

        const bool do_launch =
            (s.plunger.held_ticks > 0 && !held && in_zone) ||
            (s.plunger.auto_launch && in_zone && s.plunger.auto_timer > s.plunger.auto_delay_ticks);
        if (do_launch) {
            Ball& b = s.balls[zone_ball];
            const float q =
                s.plunger.auto_launch
                    ? 1.0f
                    : std::min(1.0f, float(s.plunger.held_ticks) / s.plunger.charge_ticks);
            const float v_launch = s.plunger.max_speed * (0.2f + 0.8f * q);
            b.vel += s.plunger.lane_dir * v_launch;
            b.vel = clamp_speed(b.vel);

            absorb(s, s.tick, SimEventType::BallLaunched, 0xFFFF);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::BallLaunched);
            ev.element = 0xFFFF;
            ev.x = b.pos.x;
            ev.y = b.pos.y;
            ev.a = q;
            ev.data = b.index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);

            s.plunger.held_ticks = 0;
            s.plunger.auto_timer = 0;
        } else if (!held) {
            s.plunger.held_ticks = 0; // release with no ball resets silently
        }
    }

    // ---- Outholes (08 §6.15): capsule radius 0.02 around a→b. ----
    for (const OutholeRegion& hole : s.outholes) {
        for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
            Ball& b = s.balls[bi];
            if (!b.live || b.mode != BallMode::Free || b.layer != 0) {
                continue;
            }
            // Point-to-segment distance.
            const Vec2 ab = hole.b - hole.a;
            const float t =
                std::clamp(dot(b.pos - hole.a, ab) / std::max(length_sq(ab), 1e-12f), 0.0f, 1.0f);
            const Vec2 closest = hole.a + ab * t;
            if (length(b.pos - closest) >= kOutholeRadius) {
                continue;
            }
            b.live = false; // drained: slot freed, trough count grows
            ++s.trough_balls;

            absorb(s, s.tick, SimEventType::Drain, 0xFFFE);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::Drain);
            ev.element = 0xFFFE;
            ev.x = b.pos.x;
            ev.y = b.pos.y;
            ev.a = 0.0f;
            ev.data = b.index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
        }
    }

    // ---- Trough serve (M5 basic loop: drain → auto-serve next ball). ----
    bool any_free = false;
    for (const Ball& b : s.balls) {
        if (b.live && b.mode == BallMode::Free) {
            any_free = true;
            break;
        }
    }
    if (any_free || s.trough_balls <= 0 || !s.has_plunger) {
        s.serve_delay_ticks = 0;
    } else {
        ++s.serve_delay_ticks;
        if (s.serve_delay_ticks >= kServeDelayTicks) {
            s.serve_delay_ticks = 0;
            for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
                Ball& b = s.balls[bi];
                if (b.live) {
                    continue;
                }
                --s.trough_balls; // only once a free slot is confirmed
                b.index = bi;
                b.live = true;
                b.mode = BallMode::Free;
                b.layer = 0;
                b.pos = s.plunger.pos + s.plunger.lane_dir * (kBallRadius + 0.002f);
                b.vel = {0.0f, 0.0f};
                b.omega_z = 0.0f;
                b.last_safe_pos = b.pos;

                absorb(s, s.tick, SimEventType::BallServed, 0xFFFD);
                SimEvent ev;
                ev.tick = s.tick;
                ev.type = uint16_t(SimEventType::BallServed);
                ev.element = 0xFFFD;
                ev.x = b.pos.x;
                ev.y = b.pos.y;
                ev.a = 0.0f;
                ev.data = b.index;
                s.render_ring.push(s.tick, ev);
                s.game_ring.push(s.tick, ev);
                break;
            }
        }
    }
}

void Solver::step_elements(SimState& s) {
    // --- Slingshots (§6.2): resolved face contact with −u_n >= 0.4 while
    // off cooldown → post-resolution kick along the active face normal.
    for (SlingshotElem& sl : s.slingshots) {
        if (sl.kick_visual_ticks > 0) {
            --sl.kick_visual_ticks;
        }
        if (sl.common.cooldown_left > 0) {
            --sl.common.cooldown_left;
            continue;
        }
        for (size_t ci = 0; ci < contact_log_n_; ++ci) {
            const LoggedContact& c = contact_log_[ci];
            if (c.element != sl.common.table_id || c.approach < 0.4f) {
                continue;
            }
            Ball* ball = nullptr;
            for (Ball& b : s.balls) {
                if (b.live && b.index == c.ball) {
                    ball = &b;
                    break;
                }
            }
            if (ball == nullptr) {
                continue;
            }
            // Kick: keep tangential motion, force normal speed >= kick.
            const Vec2 t = perp(sl.face_normal);
            const float v_t = dot(ball->vel, t);
            const float v_n = dot(ball->vel, sl.face_normal);
            const float out_n = std::max(v_n, sl.kick_speed);
            ball->vel = sl.face_normal * out_n + t * v_t;
            ball->vel = clamp_speed(ball->vel);
            sl.common.cooldown_left = sl.common.cooldown_ticks;
            sl.kick_visual_ticks = kSlingArmVisualTicks;
            emit_element_event(s, SimEventType::SwitchHit, sl.common.table_id, *ball, c.approach);
            break;
        }
    }

    // --- Pop bumpers (§6.3): any resolved contact off cooldown → radial
    // kick with deterministic rng_sim jitter.
    for (PopBumperElem& pop : s.pop_bumpers) {
        if (pop.flash_ticks > 0) {
            --pop.flash_ticks;
        }
        if (pop.common.cooldown_left > 0) {
            --pop.common.cooldown_left;
            continue;
        }
        for (size_t ci = 0; ci < contact_log_n_; ++ci) {
            const LoggedContact& c = contact_log_[ci];
            if (c.element != pop.common.table_id) {
                continue;
            }
            Ball* ball = nullptr;
            for (Ball& b : s.balls) {
                if (b.live && b.index == c.ball) {
                    ball = &b;
                    break;
                }
            }
            if (ball == nullptr) {
                continue;
            }
            // The contact log already proves this ball hit this pop's
            // circle; the current (post-rebound) position still gives a
            // sound radial direction.
            Vec2 d = ball->pos - pop.pos;
            const float dist = length(d);
            if (dist > 1e-6f) {
                d = d * (1.0f / dist);
            } else {
                d = {0.0f, 1.0f};
            }
            const float delta = (s.rng_sim.next_float() * 2.0f - 1.0f) * 0.12f;
            const float cs = std::cos(delta);
            const float sn = std::sin(delta);
            const Vec2 kicked{d.x * cs - d.y * sn, d.x * sn + d.y * cs};
            ball->vel += kicked * pop.kick_speed;
            ball->vel = clamp_speed(ball->vel);
            pop.common.cooldown_left = pop.common.cooldown_ticks;
            pop.flash_ticks = kPopFlashTicks;
            emit_element_event(
                s, SimEventType::SwitchHit, pop.common.table_id, *ball, length(ball->vel));
            break;
        }
    }

    // --- Standup targets (§6.4): facing-side contact >= min_speed.
    for (StandupTargetElem& st : s.standups) {
        if (st.common.cooldown_left > 0) {
            --st.common.cooldown_left;
            continue;
        }
        for (size_t ci = 0; ci < contact_log_n_; ++ci) {
            const LoggedContact& c = contact_log_[ci];
            if (c.element != st.common.table_id || c.approach < st.min_speed) {
                continue;
            }
            // Facing side only: the hit normal must oppose the facing.
            Ball* ball = nullptr;
            for (Ball& b : s.balls) {
                if (b.live && b.index == c.ball) {
                    ball = &b;
                    break;
                }
            }
            if (ball == nullptr) {
                continue;
            }
            const Vec2 rel = ball->pos - (st.face_a + st.face_b) * 0.5f;
            if (dot(rel, st.face_normal) < 0.0f) {
                continue; // back-side contact never triggers (§6.4)
            }
            st.common.cooldown_left = st.common.cooldown_ticks;
            emit_element_event(s, SimEventType::SwitchHit, st.common.table_id, *ball, c.approach);
            break;
        }
    }

    // --- Rollovers (§6.8): capsule enter with hysteresis; no collider.
    for (RolloverElem& ro : s.rollovers) {
        for (Ball& b : s.balls) {
            if (!b.live || b.mode != BallMode::Free || b.layer != ro.common.layer) {
                continue;
            }
            const float d = point_segment_distance(b.pos, ro.a, ro.b);
            if (ro.armed) {
                if (d < 0.012f) {
                    ro.armed = false;
                    emit_element_event(
                        s, SimEventType::SwitchHit, ro.common.table_id, b, length(b.vel));
                    emit_element_event(s, SimEventType::RolloverEvent, ro.common.table_id, b, 0.0f);
                }
            } else if (d > 0.016f) {
                ro.armed = true; // exit capsule re-arm
            }
        }
    }

    // --- Gates (§6.7): pass detection with hysteresis (blocking happens
    // as a dynamic collider inside find_earliest).
    for (GateElem& g : s.gates) {
        for (Ball& b : s.balls) {
            if (!b.live || b.mode != BallMode::Free || b.layer != g.common.layer) {
                continue;
            }
            const Vec2 rel = b.pos - g.a;
            const float along = dot(rel, g.face_normal); // signed side
            const Vec2 seg = g.b - g.a;
            const float u =
                std::clamp(dot(rel, seg) / std::max(length_sq(seg), 1e-12f), 0.0f, 1.0f);
            const bool within_span = u > 0.0f && u < 1.0f;
            const float dist = point_segment_distance(b.pos, g.a, g.b);
            if (g.switch_armed) {
                if (within_span && std::abs(along) < kBallRadius && dist < kBallRadius + 0.002f) {
                    // A pass (not a block): the crossing fired.
                    if (g.state != GateState::Closed) {
                        g.switch_armed = false;
                        emit_element_event(
                            s, SimEventType::SwitchHit, g.common.table_id, b, length(b.vel));
                    }
                }
            } else if (dist > 0.03f) {
                g.switch_armed = true; // §6.7 re-arm distance
            }
        }
    }

    // --- Spinners (§6.6): crossing with speed gate; plate model.
    for (SpinnerElem& sp : s.spinners) {
        for (Ball& b : s.balls) {
            if (!b.live || b.mode != BallMode::Free || b.layer != sp.common.layer) {
                continue;
            }
            const Vec2 rel = b.pos - sp.a;
            const float along = dot(rel, sp.face_normal);
            const Vec2 seg = sp.b - sp.a;
            const float u =
                std::clamp(dot(rel, seg) / std::max(length_sq(seg), 1e-12f), 0.0f, 1.0f);
            if (u <= 0.0f || u >= 1.0f) {
                continue; // outside the plate span
            }
            const float s_pass = dot(b.vel, sp.face_normal);
            const bool on_plate = std::abs(along) < kBallRadius;
            if (sp.crossing_armed && on_plate) {
                sp.crossing_armed = false;
                sp.last_ball = b.index;
                if (std::abs(s_pass) >= 0.15f) {
                    // Spin-up + one-shot ball slowdown (plate inertia).
                    const float side = s_pass > 0.0f ? 1.0f : -1.0f;
                    sp.plate_omega = side * std::abs(s_pass) * 25.0f;
                    if (std::abs(s_pass) > 0.12f) {
                        b.vel -= sp.face_normal * (side * 0.12f);
                    }
                }
                // Slow crossings are handled as a steel wall inside the
                // CCD (conditional dynamic collider, find_earliest).
            } else if (!sp.crossing_armed && std::abs(along) > kBallRadius + 0.005f) {
                sp.crossing_armed = true; // clear of the plate: re-arm
            }
        }

        // Plate model (every tick): friction decay + revolution events.
        if (std::abs(sp.plate_omega) >= 0.5f) {
            sp.plate_angle += sp.plate_omega * kTickDt;
            sp.rev_angle_acc += std::abs(sp.plate_omega) * kTickDt;
            sp.plate_omega *= kSpinnerDecayPerTick; // 0.55/s (§6.6)
            if (sp.rev_angle_acc >= 2.0f * float(kPiF)) {
                sp.rev_angle_acc -= 2.0f * float(kPiF);
                for (Ball& b : s.balls) {
                    if (b.live) {
                        emit_element_event(s, SimEventType::SwitchHit, sp.common.table_id, b, 0.0f);
                        emit_element_event(s,
                                           SimEventType::SpinnerSpin,
                                           sp.common.table_id,
                                           b,
                                           std::abs(sp.plate_omega) * 60.0f / (2.0f * float(kPiF)));
                        break;
                    }
                }
            }
        } else {
            sp.plate_omega = 0.0f;
        }
    }
}

void Solver::pushout(SimState& s) {
    // §3.8 fallback depenetration: deepest-overlap position correction,
    // never touching velocity or spin.
    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Free) {
            continue;
        }
        for (int iter = 0; iter < 4; ++iter) {
            s.grid.query(ball.pos, ball.pos, kBallRadius, ball.layer, candidates_);
            float depth = 0.0f;
            Vec2 push_n{};
            for (uint32_t ci : candidates_) {
                const Collider& c = s.colliders[ci];
                Vec2 n{};
                float sep = 0.0f;
                switch (c.kind) {
                case Collider::Kind::Point: {
                    const Vec2 d = ball.pos - c.a;
                    const float dist = length(d);
                    sep = dist - (c.radius + kBallRadius);
                    n = dist > 0.0f ? d * (1.0f / dist) : Vec2{0.0f, 1.0f};
                    break;
                }
                case Collider::Kind::Segment: {
                    const Vec2 d = c.b - c.a;
                    const float len = length(d);
                    if (len <= 0.0f) {
                        continue;
                    }
                    const Vec2 dn = perp(d * (1.0f / len));
                    const float sn = dot(ball.pos - c.a, dn);
                    sep = std::fabs(sn) - kBallRadius;
                    n = sn >= 0.0f ? dn : dn * -1.0f;
                    break;
                }
                case Collider::Kind::Arc:
                default:
                    continue; // arc tips carry point colliders already
                }
                if (sep < 0.0f && -sep > depth) {
                    depth = -sep;
                    push_n = n;
                }
            }
            if (depth <= 0.0f) {
                break;
            }
            ball.pos += push_n * (depth + kSkin);
        }
    }
}

} // namespace tb::sim

namespace tb::sim {

uint64_t state_hash(const SimState& s) {
    // Canonical serialization (16-testing-ci.md §2.4.1): balls in index
    // order — live/mode/layer bytes then raw IEEE-754 bit patterns of
    // pos/vel/spin — both RNG stream states, then the event-sequence
    // accumulator as its final 8 bytes.
    uint64_t h = kFnvOffset;
    auto mix = [&h](const void* data, size_t len) { h = fnv1a64(data, len, h); };

    for (int i = 0; i < kMaxBalls; ++i) {
        const Ball& b = s.balls[i];
        const unsigned char meta[3] = {static_cast<unsigned char>(b.live ? 1 : 0),
                                       static_cast<unsigned char>(b.mode),
                                       static_cast<unsigned char>(b.layer)};
        mix(meta, sizeof(meta));
        mix(&b.pos.x, 4);
        mix(&b.pos.y, 4);
        mix(&b.vel.x, 4);
        mix(&b.vel.y, 4);
        mix(&b.omega_z, 4);
    }

    mix(&s.rng_sim, sizeof(s.rng_sim));
    mix(&s.rng_script, sizeof(s.rng_script));
    mix(&s.event_seq_hash, sizeof(s.event_seq_hash));
    return h;
}

void make_synthetic_scene(SimState& s, uint64_t game_seed) {
    // The §2.9 synthetic perf scene: closed border, 8×10 rubber post
    // lattice, four steel arcs, four balls. Built programmatically —
    // no table.json, no Lua.
    constexpr float kWidth = 0.52f;
    constexpr float kHeight = 1.04f;
    uint16_t next_sub = 0;

    auto add = [&](Collider c) {
        c.element_id = 0;
        c.sub_index = next_sub++;
        c.layer = 0;
        s.colliders.push_back(c);
    };

    // Closed border: four wood segments.
    const Vec2 bl{0.0f, 0.0f}, br{kWidth, 0.0f}, tr{kWidth, kHeight}, tl{0.0f, kHeight};
    Collider border;
    border.kind = Collider::Kind::Segment;
    border.material = MaterialId::Wood;
    Vec2 loop[5] = {bl, br, tr, tl, bl};
    for (int i = 0; i < 4; ++i) {
        border.a = loop[i];
        border.b = loop[i + 1];
        add(border);
    }

    // Rubber post lattice: x = 0.05+0.06·i (0..7), y = 0.12+0.08·j (0..9).
    Collider post;
    post.kind = Collider::Kind::Point;
    post.material = MaterialId::Rubber;
    post.radius = 0.008f;
    for (int j = 0; j < 10; ++j) {
        for (int i = 0; i < 8; ++i) {
            post.a = {0.05f + 0.06f * float(i), 0.12f + 0.08f * float(j)};
            add(post);
        }
    }

    // Four steel arcs: lower half-circles (180°→360° CCW).
    Collider arc;
    arc.kind = Collider::Kind::Arc;
    arc.material = MaterialId::Steel;
    const Vec2 centers[2] = {{0.16f, 0.98f}, {0.36f, 0.98f}};
    const float radii[2] = {0.05f, 0.08f};
    for (const Vec2& c : centers) {
        for (float r : radii) {
            arc.a = c;
            arc.radius = r;
            arc.a0 = float(kPi);       // 180°
            arc.a1 = float(2.0 * kPi); // 360°
            add(arc);
        }
    }

    s.width = kWidth;
    s.height = kHeight;
    s.grid.build(s.colliders, s.width, s.height);

    // Four balls at y = 1.00, velocity components uniform in [−3, +3]
    // drawn by PCG32(seed = 99). Spawn positions are penetration-free.
    Pcg32 spawn;
    spawn.seed(99, 54);
    const float xs[4] = {0.10f, 0.20f, 0.30f, 0.40f};
    for (int i = 0; i < 4; ++i) {
        Ball& b = s.balls[i];
        b.index = uint8_t(i);
        b.live = true;
        b.mode = BallMode::Free;
        b.layer = 0;
        b.pos = {xs[i], 1.00f};
        b.vel = {(spawn.next_float() * 6.0f - 3.0f), (spawn.next_float() * 6.0f - 3.0f)};
        b.vel = clamp_speed(b.vel);
        b.last_safe_pos = b.pos;
    }

    s.seeded = true;
}

} // namespace tb::sim
