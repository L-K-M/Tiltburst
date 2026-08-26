#include "sim/solver.h"

#include <algorithm>
#include <cmath>

namespace tb::sim {

namespace {

constexpr float kPi = 3.14159265358979f;

inline Vec2 clamp_speed(Vec2 v) {
    const float speed_sq = length_sq(v);
    if (speed_sq > kMaxSpeed * kMaxSpeed) {
        const float s = kMaxSpeed / std::sqrt(speed_sq);
        return v * s;
    }
    return v;
}

// §4.2: velocity-dependent restitution.
inline float restitution_curve(const SimState& s, float e, float approach) {
    if (approach < kRestSpeed) {
        return 0.0f;
    }
    return e / (1.0f + s.restitution_falloff * std::max(0.0f, approach - s.restitution_soft));
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

float Solver::resolve_static(SimState& s, Ball& ball, Vec2 n, const Material& mat) {
    // §4.1 impulse equations; surface velocity zero for static colliders.
    const Vec2 t = perp(n);

    // Ball surface-point velocity: v_p = v − r·omega_z·perp(n̂); u = v_p.
    const Vec2 vp = ball.vel - kBallRadius * ball.omega_z * perp(n);
    const float u_n = dot(vp, n);
    if (u_n >= 0.0f) {
        return 0.0f; // separating — never apply impulses
    }
    const float u_t = dot(vp, t);

    const float approach = -u_n;
    const float e_eff = restitution_curve(s, mat.restitution, approach);
    const float j_n = -(1.0f + e_eff) * u_n * kBallMass;

    // Tangential effective mass: 1/m_t = 7/(2m) → m_t = 2m/7.
    const float j_stick = -(2.0f * kBallMass / 7.0f) * u_t;
    float j_t;
    if (std::fabs(j_stick) <= mat.mu_s * j_n) {
        j_t = j_stick;
    } else {
        j_t = -std::copysign(mat.mu_k * j_n, u_t);
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

void Solver::step(SimState& s, const TickInput&) {
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

        if (best.kind == Contact::Static) {
            Ball& ball = s.balls[best.ball];
            const float speed = length(ball.vel);
            if (speed > 0.0f) {
                const float t_back = std::min(kSkin / speed, t_cur);
                ball.pos -= ball.vel * t_back; // keep kSkin separation
            }

            const float approach =
                resolve_static(s, ball, best.normal, material_row(best.collider->material));

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
