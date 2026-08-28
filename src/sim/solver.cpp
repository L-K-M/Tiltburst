#include "sim/solver.h"

#include "core/log.h"
#include "sim/ramp.h"
#include "sim/script_host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
static_assert(kSpinnerDecayPerTick > 0.999402f && kSpinnerDecayPerTick < 0.999403f,
              "kSpinnerDecayPerTick must stay exp(ln(0.55) * 0.001)");

void emit_bank_event(SimState& s, SimEventType type, uint16_t element);
void emit_lock_event(SimState& s, const BallLockElem& lock);
void request_bank_reset(SimState& s, DropBankElem& bank);
uint8_t serve_ball(SimState& s);
void record_tick_event(SimState& s, const SimEvent& ev);

// Serve one trough ball onto the plunger (§6.15 kinematics); returns
// the spawned ball index (0xFF = nothing served). No event: callers
// decide whether BallServed applies (§3.5 fires none).
uint8_t serve_ball(SimState& s) {
    if (s.trough_balls <= 0 || !s.has_plunger) {
        return 0xFF;
    }
    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& b = s.balls[bi];
        if (b.live) {
            continue;
        }
        --s.trough_balls;
        b.index = bi;
        b.live = true;
        b.mode = BallMode::Free;
        b.layer = 0;
        b.pos = s.plunger.pos + s.plunger.lane_dir * (kBallRadius + 0.002f);
        b.vel = {0.0f, 0.0f};
        b.omega_z = 0.0f;
        b.last_safe_pos = b.pos;
        return bi;
    }
    return 0xFF;
}

// Per-tick emission-order log for the script host (§2.2 phase 2). Filled
// only while a host is attached; the Collision audio event never records.
// (record_tick_event itself lives below, after the sound emitters.)

// 12-audio.md §7.2: emit a purpose-mapped SoundEvent. Velocity is
// impact-derived (impact_speed / 8 m/s) for impact purposes, 1.0 for
// state purposes; pan derives from the ball's x position (§4.1).
// No queue, a disabled purpose, or a full ring (drop-NEW, §4.1) means
// silence — audio never feeds back into simulation state.
void emit_sound(SimState& s, int purpose, const Ball* ball, float impact_speed) {
    if (s.sound_queue == nullptr || purpose < 0 || purpose >= SimState::kSoundPurposeCount) {
        return;
    }
    const int patch = s.sound_purpose_patch[purpose];
    if (patch < 0) {
        return; // disabled ("none") or unmapped
    }
    SoundEvent ev;
    ev.tick = uint32_t(s.tick);
    ev.patch = uint16_t(patch);
    ev.flags = 0;
    ev.velocity = std::clamp(impact_speed / 8.0f, 0.0f, 1.0f);
    ev.pan = ball != nullptr ? std::clamp(2.0f * ball->pos.x / s.width - 1.0f, -1.0f, 1.0f) * 0.6f
                             : 0.0f;
    if (!s.sound_queue->push(ev)) {
        ++s.sounds_dropped; // §4.1: drop-NEW on a full ring
    }
}

// Position variant for element-sited sounds with no ball in hand
// (kicker eject, lock capture): pan derives from the element x.
void emit_sound_x(SimState& s, int purpose, float x, float impact_speed) {
    if (s.sound_queue == nullptr) {
        return;
    }
    const int patch = purpose >= 0 && purpose < SimState::kSoundPurposeCount
                          ? s.sound_purpose_patch[purpose]
                          : -1;
    if (patch < 0) {
        return;
    }
    SoundEvent ev;
    ev.tick = uint32_t(s.tick);
    ev.patch = uint16_t(patch);
    ev.velocity = std::clamp(impact_speed / 8.0f, 0.0f, 1.0f);
    ev.pan = std::clamp(2.0f * x / s.width - 1.0f, -1.0f, 1.0f) * 0.6f;
    if (!s.sound_queue->push(ev)) {
        ++s.sounds_dropped;
    }
}

void record_tick_event(SimState& s, const SimEvent& ev) {
    if (s.script == nullptr && s.fsm_step == nullptr) {
        return;
    }
    if (s.tick_event_n >= SimState::kTickEventCap) {
        ++s.tick_events_dropped;
        if (!s.tick_event_drop_warned) {
            s.tick_event_drop_warned = true;
            TB_LOG_WARN("script",
                        "tick event log overflowed (cap {}): further events "
                        "dropped this session — rules/goldens may diverge",
                        SimState::kTickEventCap);
        }
        return;
    }
    s.tick_events[s.tick_event_n++] = ev;
}

void emit_element_event(
    SimState& s, SimEventType type, uint16_t element, const Ball& ball, float payload);

// Drop-bank faces stop colliding the tick their target leaves UP (§6.5).
bool collider_enabled(const SimState& s, uint32_t collider_idx) {
    for (const DropBankElem& bank : s.drop_banks) {
        for (const DropBankElem::Target& t : bank.targets) {
            if (t.collider_idx == collider_idx && t.state != DropTargetState::Up) {
                return false;
            }
        }
    }
    return true;
}

Vec2 captive_pos(const CaptiveBallElem& cap) {
    return cap.a + cap.axis * cap.s_c;
}

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

// Framework serve (§6.15 + §4.2): spawn AND emit BallServed — the M10
// framework's serve windows close on that event (11 §2.5).
void serve_ball_notified(SimState& s) {
    const uint8_t spawned = serve_ball(s);
    if (spawned != 0xFF) {
        absorb(s, s.tick, SimEventType::BallServed, 0xFFFD);
        SimEvent ev;
        ev.tick = s.tick;
        ev.type = uint16_t(SimEventType::BallServed);
        ev.element = 0xFFFD;
        // Attribute to the SPAWNED ball: with multiball live the
        // lowest live index is some other ball.
        ev.x = s.balls[spawned].pos.x;
        ev.y = s.balls[spawned].pos.y;
        ev.data = s.balls[spawned].index;
        s.render_ring.push(s.tick, ev);
        s.game_ring.push(s.tick, ev);
        record_tick_event(s, ev);
    }
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
    record_tick_event(s, ev);
}

void emit_bank_event(SimState& s, SimEventType type, uint16_t element) {
    absorb(s, s.tick, type, element);
    SimEvent ev;
    ev.tick = s.tick;
    ev.type = uint16_t(type);
    ev.element = element;
    s.render_ring.push(s.tick, ev);
    s.game_ring.push(s.tick, ev);
    record_tick_event(s, ev);
}

void emit_lock_event(SimState& s, const BallLockElem& lock) {
    emit_sound_x(s, int(SoundPurpose::BallLock), lock.pos.x, 8.0f);
    absorb(s, s.tick, SimEventType::BallLockCapture, lock.common.table_id);
    SimEvent ev;
    ev.tick = s.tick;
    ev.type = uint16_t(SimEventType::BallLockCapture);
    ev.element = lock.common.table_id;
    ev.a = float(lock.held); // payload {lock_id, count}: count = held
    s.render_ring.push(s.tick, ev);
    s.game_ring.push(s.tick, ev);
    record_tick_event(s, ev);
}

void request_bank_reset(SimState& s, DropBankElem& bank) {
    (void)s;
    for (DropBankElem::Target& t : bank.targets) {
        if (t.state != DropTargetState::Down) {
            continue;
        }
        t.state = DropTargetState::Raising;
        t.anim_ticks = 250;
    }
    bank.last_not_complete = true; // the bank can complete again
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

    // ball_ball purpose (12 §7.2): 30 ms rate limit.
    if (-u_n > 0.5f && s.tick - s.ball_sound_tick >= 30) {
        s.ball_sound_tick = uint32_t(s.tick);
        emit_sound(s, int(SoundPurpose::BallBall), &a, -u_n);
    }

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
        best.kind = Contact::Static;
        best.toi = toi;
        best.ball = ball_idx;
        best.ball2 = ball_idx;
        best.captive_idx = 0;
        best.normal = normal;
        best.collider = col;
        best.flipper = nullptr;
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
            if (!collider_enabled(s, ci)) {
                continue; // dropped target (§6.5)
            }
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

        // Captive balls (§6.13): swept vs the captive's current position;
        // resolved with the admissible-motion impulse.
        for (uint8_t cap_i = 0; cap_i < s.captives.size(); ++cap_i) {
            CaptiveBallElem& cap = s.captives[cap_i];
            if (ball.layer != cap.common.layer) {
                continue;
            }
            const Vec2 cpos = captive_pos(cap);
            SweepHit hit;
            if (sweep_circle_vs_point(
                    ball.pos, ball.vel, kBallRadius, cpos, kBallRadius, window, hit)) {
                const float toi = t_cur + hit.toi;
                const bool better = best.kind == Contact::None || toi < best.toi - kToiEps;
                if (better) {
                    best.toi = toi;
                    best.kind = Contact::Captive;
                    best.ball = bi;
                    best.ball2 = bi;
                    best.captive_idx = cap_i;
                    best.normal = hit.normal;
                    best.collider = nullptr;
                    best.flipper = nullptr;
                }
                (void)0;
            }
        }

        // RAMP balls were projected above; the 2-D loop skips them.

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
                    best.kind = Contact::Pair;
                    best.toi = toi;
                    best.ball = bi;
                    best.ball2 = bj;
                    best.captive_idx = 0;
                    best.normal = hit.normal * -1.0f;
                    best.collider = nullptr;
                    best.flipper = nullptr;
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

    // Phase 1 pre (10-scripting.md §2.2): apply the physical actions
    // latched during tick n−1's handlers; physics state stays immutable
    // while scripts run.
    if (s.script != nullptr || s.fsm_step != nullptr) {
        s.tick_event_n = 0; // BEFORE actions: serve/eject actions record
                            // events this same tick (§2.2 phase 1)
    }
    if (s.script != nullptr) {
        apply_script_actions(s, s.script->pending_actions());
        s.script->pending_actions().clear();
    }

    // Nudge edges + tilt-bob integration (08 §7, M10). Edge-triggered:
    // a new press starts a new envelope and kicks the bob; holding does
    // nothing. Levels 1/2/3 per 08 §7.1 (side 0.15/0.25/0.35, front
    // 0.20/0.30/0.40 m/s).
    if (input != nullptr) {
        constexpr uint32_t kNudgeLeftBit = 5; // 05 §9.1 action indices
        constexpr uint32_t kNudgeRightBit = 6;
        constexpr uint32_t kNudgeUpBit = 7;
        const uint32_t rising = input->buttons & ~s.input_prev_buttons;
        // 1..3 (settings; replay header). Anything out of range maps to
        // the MIDDLE level — boundary clamping would turn garbage like
        // 7 into the STRONGEST nudges.
        const int level = (s.nudge_level >= 1 && s.nudge_level <= 3) ? s.nudge_level : 2;
        auto nudge_dv = [level](uint32_t bit) -> std::pair<Vec2, Vec2> {
            // Returns {ball d_hat·dv, cab d_hat·dv}: the button names the
            // direction the cabinet is shoved; balls accelerate the
            // opposite way relative to the table (08 §7.1).
            const float side = level == 1 ? 0.15f : (level == 2 ? 0.25f : 0.35f);
            const float front = level == 1 ? 0.20f : (level == 2 ? 0.30f : 0.40f);
            switch (bit) {
            case kNudgeLeftBit:
                return {{+side, 0.0f}, {-side, 0.0f}};
            case kNudgeRightBit:
                return {{-side, 0.0f}, {+side, 0.0f}};
            default:
                return {{0.0f, +front}, {0.0f, -front}};
            }
        };
        for (const uint32_t bit : {kNudgeLeftBit, kNudgeRightBit, kNudgeUpBit}) {
            if (((rising >> bit) & 1u) == 0u) {
                continue;
            }
            const auto [dv_ball, dv_cab] = nudge_dv(bit);
            // Envelope: 30 ms half-sine, integral 0.0191·A = dv, so
            // A = dv / (2·0.030/π) (08 §7.1).
            const float kEnvelope = 2.0f * 0.030f / float(kPi); // 0.0191 s
            bool placed = false;
            for (SimState::NudgeEnvelope& e : s.nudge_envelopes) {
                if (e.ticks_left == 0) {
                    e.ax = dv_ball.x / kEnvelope;
                    e.ay = dv_ball.y / kEnvelope;
                    e.ticks_left = 30;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                TB_LOG_WARN("sim", "nudge envelope cap reached; nudge dropped");
            }
            // Bob kick (08 §7.2) + abuse accumulator (08 §7.3).
            s.tilt.v = s.tilt.v + dv_cab;
            s.tilt.abuse_acc =
                std::max(0.0f, s.tilt.abuse_acc + (std::fabs(dv_cab.x) + std::fabs(dv_cab.y)));
        }
        s.input_prev_buttons = input->buttons;
    }
    // Bob: p̈ = −ω_n²·p − 2ζω_n·ṗ, ω_n = 9, ζ = 0.15 (08 §7.2); abuse
    // leaks 0.15 m/s per second. Both integrated with semi-implicit
    // Euler at the tick rate, matching the sim's integrator.
    {
        constexpr float kOmegaN = 9.0f, kZeta = 0.15f;
        const Vec2 a = s.tilt.p * (-kOmegaN * kOmegaN) - s.tilt.v * (2.0f * kZeta * kOmegaN);
        s.tilt.v = s.tilt.v + a * 0.001f;
        s.tilt.p = s.tilt.p + s.tilt.v * 0.001f;
        s.tilt.abuse_acc = std::max(0.0f, s.tilt.abuse_acc - 0.15f * 0.001f);

        // Threshold crossings, each independently armed, re-arming at
        // 0.7× its own value (08 §7.2). Emission order within a tick:
        // warn, hard, abuse (threshold order).
        auto emit_danger = [&](SimEvent& ev, float magnitude, uint16_t source) {
            ++s.tilt.crossings;
            ev.type = uint16_t(SimEventType::DangerThreshold);
            ev.element = 0xFFFF;
            ev.a = magnitude;
            ev.data = uint32_t(source) | (uint32_t(s.tilt.crossings) << 16);
            absorb(s, s.tick, SimEventType::DangerThreshold, 0xFFFF);
            // Rings too — the full emission pattern (serve_ball_notified,
            // emit_bank_event): tilt-warning audio/HUD consumers read the
            // rings, not the script log. absorb() already ran, so the
            // event-sequence hash is unchanged.
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
            record_tick_event(s, ev);
        };
        SimEvent ev;
        ev.tick = s.tick;
        const float mag = std::sqrt(dot(s.tilt.p, s.tilt.p));
        if (s.tilt.warn_armed && mag >= s.tilt.warn_m) {
            s.tilt.warn_armed = false;
            emit_danger(ev, mag, 0);
        }
        if (s.tilt.hard_armed && mag >= s.tilt.hard_m) {
            s.tilt.hard_armed = false;
            emit_danger(ev, mag, 1);
        }
        if (s.tilt.abuse_armed && s.tilt.abuse_acc >= s.tilt.abuse_mps) {
            s.tilt.abuse_armed = false;
            emit_danger(ev, s.tilt.abuse_acc, 2);
        }
        s.tilt.warn_armed = s.tilt.warn_armed || mag < 0.7f * s.tilt.warn_m;
        s.tilt.hard_armed = s.tilt.hard_armed || mag < 0.7f * s.tilt.hard_m;
        s.tilt.abuse_armed = s.tilt.abuse_armed || s.tilt.abuse_acc < 0.7f * s.tilt.abuse_mps;
    }

    // Step 2 — flipper state update (§5.2), id order; (theta_start,
    // omega) held constant for CCD this tick. Tilt clears the gate
    // (11-game-framework.md §5: flippers dead after tilt).
    for (Flipper& f : s.flippers) {
        bool pressed = f.enabled && s.flippers_enabled && input != nullptr &&
                       ((input->buttons >> f.params.action) & 1u) != 0u;
        if (pressed && !f.sound_prev_pressed && f.enabled && s.flippers_enabled) {
            emit_sound_x(
                s, int(SoundPurpose::Flipper), f.params.pivot.x, 8.0f); // press edge (§7.2)
        }
        f.sound_prev_pressed = pressed;
        FlipperSim::tick(f, pressed);
    }

    // Step 3 — forces + velocity update (FREE balls, index order).
    const float slope_rad = s.slope_deg * (float(kPi) / 180.0f);
    const float g_slope = kGravity * std::sin(slope_rad);
    const float rr = s.mu_rr * kGravity * std::cos(slope_rad);

    // Active nudge envelopes apply a half-sine a(t) = A·sin(π·t/30)
    // scaled per tick (08 §7.1); overlapping nudges sum. Applied to FREE
    // balls only (all layers; never RAMP or CAPTURED).
    Vec2 nudge_a{};
    for (SimState::NudgeEnvelope& e : s.nudge_envelopes) {
        if (e.ticks_left == 0) {
            continue;
        }
        const float phase = float(30u - e.ticks_left) / 30.0f;
        const float scale = std::sin(float(kPi) * phase);
        nudge_a = nudge_a + Vec2{e.ax * scale, e.ay * scale};
        --e.ticks_left;
    }

    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live) {
            continue;
        }

        if (ball.mode == BallMode::Ramp) {
            // §6.10.4 1-D dynamics (step 3); position integrates in
            // step 4/5 below via the ramp projection.
            const RampPath* ramp = nullptr;
            for (const RampPath& r : s.ramps) {
                if (r.element_id == ball.ramp_elem) {
                    ramp = &r;
                    break;
                }
            }
            if (ramp != nullptr) {
                // §6.10.4 dynamics. The 2-D arc length is used as-is (the
                // sqrt(1+(dz/ds)²) correction is deliberately ignored).
                const Vec2 t_hat = ramp->tangent_at(ball.s);
                const float s_hi = std::min(ball.s + 0.001f, ramp->total_s);
                const float s_lo = std::max(ball.s - 0.001f, 0.0f);
                const float dz_ds =
                    (ramp->z_at(s_hi) - ramp->z_at(s_lo)) / std::max(s_hi - s_lo, 1e-6f);
                const float a_gravity = -kGravity * std::sin(slope_rad) * t_hat.y -
                                        kGravity * std::cos(slope_rad) * dz_ds;
                const float damping = -0.10f * ball.s_dot;
                const float fr = 0.015f * kGravity; // rolling resistance
                ball.s_dot += (a_gravity + damping) * kTickDt;
                // Friction never reverses the sign within the tick: use the
                // post-gravity sign so a within-tick reversal isn't helped.
                const float fr_dv = fr * kTickDt;
                if (std::abs(ball.s_dot) <= fr_dv) {
                    ball.s_dot = 0.0f;
                } else {
                    ball.s_dot -= std::copysign(fr_dv, ball.s_dot);
                }
                ball.s_dot = std::clamp(ball.s_dot, -kMaxSpeed, kMaxSpeed);
                ball.omega_z *= std::exp(-kSpinDamp * kTickDt);
            }
            continue;
        }
        if (ball.mode != BallMode::Free) {
            continue;
        }

        Vec2 acc{0.0f, -g_slope}; // slope gravity
        acc = acc + nudge_a;      // active nudge envelopes (08 §7.1)

        // Magnet forces (§6.12), step 3 item 2. De-energized on tilt
        // (11 §5): magnets do not energize — damping persists (eddy
        // drains kinetic energy whether or not the coil is driven).
        if (s.coils_enabled) {
            for (MagnetSim& mag : s.magnets) {
                if (mag.layer == ball.layer) {
                    acc += mag.accel(ball);
                    mag.damp(ball);
                    // §7.2: the hum retriggers every 500 ms while the
                    // field holds a ball INSIDE it (layer match alone
                    // would retrigger on distant balls).
                    if (mag.on && length(ball.pos - mag.pos) < mag.radius &&
                        s.tick - mag.hum_tick >= 500) {
                        mag.hum_tick = uint32_t(s.tick);
                        emit_sound_x(s, int(SoundPurpose::Magnet), mag.pos.x, 8.0f);
                    }
                }
            }
        } else {
            for (const MagnetSim& mag : s.magnets) {
                if (mag.layer == ball.layer) {
                    mag.damp(ball);
                }
            }
        }

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

    // Step 4/5 for RAMP balls: integrate s, project position, resolve
    // exits and fall-back (§6.10.6). RAMP balls never enter the 2-D CCD.
    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Ramp) {
            continue;
        }
        RampPath* ramp = nullptr;
        for (RampPath& r : s.ramps) {
            if (r.element_id == ball.ramp_elem) {
                ramp = &r;
                break;
            }
        }
        if (ramp == nullptr) {
            ball.mode = BallMode::Free; // orphaned bind: recover
            continue;
        }
        ball.s += ball.s_dot * kTickDt;

        // Fall-back-off (§6.10.6): only for a stalled ball (s_dot crossed
        // or hit zero) within 0.05 m of the end it is still essentially at
        // the height of. A climbing ball near the entry never qualifies.
        const float z_here = ramp->z_at(std::clamp(ball.s, 0.0f, ramp->total_s));
        const bool stalled = std::abs(ball.s_dot) <= 0.05f;
        const bool near_zero =
            ball.s < 0.05f && stalled && std::abs(z_here - ramp->end_z[0]) <= 0.006f;
        const bool near_far = ball.s > ramp->total_s - 0.05f && stalled &&
                              std::abs(z_here - ramp->end_z[1]) <= 0.006f;
        if (near_zero || near_far) {
            const int end = near_zero ? 0 : 1;
            ball.mode = BallMode::Free;
            ball.layer = ramp->seam_layer[end] == 0xFF ? 0 : ramp->seam_layer[end];
            ball.pos = ramp->point_at(std::clamp(ball.s, 0.0f, ramp->total_s));
            const Vec2 t_hat = ramp->tangent_at(ball.s);
            ball.vel = t_hat * ball.s_dot;
            continue;
        }

        if (ball.s > ramp->total_s) {
            // Forward exit (§6.10.6).
            const Vec2 t_exit = ramp->tangent_at(ramp->total_s);
            ball.mode = BallMode::Free;
            ball.pos = ramp->point_at(ramp->total_s);
            ball.vel = t_exit * (ball.s_dot * 0.95f); // 5% exit loss
            ball.layer =
                ramp->drop_exit ? 0 : (ramp->seam_layer[1] == 0xFF ? 0 : ramp->seam_layer[1]);
            emit_element_event(
                s, SimEventType::SwitchHit, ramp->element_id, ball, std::abs(ball.s_dot));
            emit_sound(s, int(SoundPurpose::RampMade), &ball, std::abs(ball.s_dot));
            absorb(s, s.tick, SimEventType::RampMade, ramp->element_id);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::RampMade);
            ev.element = ramp->element_id;
            ev.x = ball.pos.x;
            ev.y = ball.pos.y;
            ev.data = ball.index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
            record_tick_event(s, ev);
            continue;
        }
        if (ball.s < 0.0f) {
            // Rolled back out the s = 0 end (§6.10.6).
            const Vec2 t_entry = ramp->tangent_at(0.0f);
            ball.mode = BallMode::Free;
            ball.pos = ramp->point_at(0.0f);
            ball.vel = t_entry * ball.s_dot; // s_dot < 0: exits backward
            ball.layer = ramp->seam_layer[0] == 0xFF ? 0 : ramp->seam_layer[0];
            continue;
        }

        // Constrained projection (step 5).
        ball.pos = ramp->point_at(ball.s);
    }

    // Step 6a — seam binding for FREE balls (§6.10.2), before triggers.
    for (uint8_t bi = 0; bi < kMaxBalls; ++bi) {
        Ball& ball = s.balls[bi];
        if (!ball.live || ball.mode != BallMode::Free) {
            continue;
        }
        for (RampPath& ramp : s.ramps) {
            for (int end = 0; end < 2; ++end) {
                const uint8_t seam = ramp.seam_layer[end];
                if (seam == 0xFF || seam != ball.layer) {
                    continue;
                }
                // Crossing the seam segment (perpendicular, width long)?
                const Vec2 rel = ball.pos - ramp.seam_center[end];
                const Vec2 into = ramp.seam_into[end];
                const float along_into = dot(rel, into);
                const float lateral = std::abs(dot(rel, Vec2{-into.y, into.x}));
                if (lateral > ramp.width * 0.5f + kBallRadius) {
                    continue;
                }
                // Crossing: the pre-tick center was on the outside, the
                // post-tick center is on the inside (strict sign change —
                // a ball leaving must never rebind).
                const Vec2 pre_rel = ball.pos - ball.vel * kTickDt - ramp.seam_center[end];
                const float pre_along = dot(pre_rel, into);
                if (!(pre_along <= 0.0f && along_into > 0.0f)) {
                    continue;
                }
                const float v_along = dot(ball.vel, into);
                if (v_along < 0.1f) {
                    continue; // speed gate (§6.10.2)
                }
                const float cos_a = v_along / std::max(length(ball.vel), 1e-9f);
                if (cos_a < std::cos(50.0f * float(3.14159265358979 / 180.0))) {
                    continue; // alignment gate
                }
                ball.mode = BallMode::Ramp;
                ball.ramp_elem = ramp.element_id;
                ball.s = end == 0 ? 0.0f : ramp.total_s;
                // §6.10.2 sign: s_dot > 0 toward the far end from s=0;
                // entering at S rides with negative s_dot.
                ball.s_dot = (end == 0 ? v_along : -v_along) * 0.95f;
                ball.pos = ramp.seam_center[end];
                break;
            }
            if (ball.mode == BallMode::Ramp) {
                break;
            }
        }
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

            // wall_hit purpose (12 §7.2): >1.5 m/s, 30 ms/ball.
            if (approach > 1.5f && s.tick - s.wall_sound_tick[best.ball] >= 30) {
                s.wall_sound_tick[best.ball] = uint32_t(s.tick);
                emit_sound(s, int(SoundPurpose::WallHit), &ball, approach);
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
            record_tick_event(s, ev);

            resolved_[best.ball]++;
        } else if (best.kind == Contact::Captive) {
            resolve_captive(s, s.balls[best.ball], s.captives[best.captive_idx], best.normal);
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
    step_lifecycle(s, input);

    // Phase 2 (10-scripting.md §2.2): dispatch this tick's sim events to
    // Lua in emission order; phase 4 timers + GC after.
    if (s.script != nullptr && s.script->scripting_active()) {
        s.script->begin_tick(s.tick);
        for (size_t i = 0; i < s.tick_event_n; ++i) {
            s.script->dispatch(s.tick_events[i]); // phase 2
        }
        if (s.fsm_step != nullptr && input != nullptr) {
            s.fsm_step(s.fsm_ctx, s, *input); // phase 3 (11 §1): GameFsm
        }
        s.script->end_tick(s.tick); // phase 4: timers + GC step
    } else if (s.fsm_step != nullptr && input != nullptr) {
        s.fsm_step(s.fsm_ctx, s, *input);
    }
    s.tick_event_n = 0;

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
            emit_sound(s, int(SoundPurpose::Launch), &b, v_launch);
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
            record_tick_event(s, ev);

            s.plunger.held_ticks = 0;
            s.plunger.auto_timer = 0;
        } else if (!held) {
            s.plunger.held_ticks = 0; // release with no ball resets silently
        }
    }

    // ---- Kickers (08 §6.9): capture zone scan. ----
    for (KickerElem& k : s.kickers) {
        if (k.held_ball != 0xFF) {
            continue; // one ball per kicker in v1 (§4.12 style)
        }
        if (!s.coils_enabled) {
            continue; // de-energized on tilt (11 §5): no NEW captures
        }
        for (Ball& b : s.balls) {
            if (!b.live || b.mode != BallMode::Free || b.layer != k.common.layer) {
                continue;
            }
            if (length(b.pos - k.pos) >= k.radius) {
                continue;
            }
            if (k.style == KickerStyle::Saucer && length(b.vel) >= 3.0f) {
                continue; // fast balls fly over a saucer (§6.9)
            }
            b.mode = BallMode::Captured;
            b.holder_elem = k.common.table_id;
            b.hold_ticks = k.capture_ticks;
            b.pos = k.pos;
            b.vel = {0.0f, 0.0f};
            b.omega_z = 0.0f;
            k.held_ball = b.index;
            k.has_hold = true;
            k.hold_ticks = k.capture_ticks;
            emit_element_event(s, SimEventType::SwitchHit, k.common.table_id, b, length(b.vel));
            absorb(s, s.tick, SimEventType::KickerEnter, k.common.table_id);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::KickerEnter);
            ev.element = k.common.table_id;
            ev.x = b.pos.x;
            ev.y = b.pos.y;
            ev.data = b.index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
            record_tick_event(s, ev);
            break;
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
            // Ball save (M7): a drain inside the window re-serves
            // instead of consuming the ball.
            if (s.ball_save.active) {
                s.ball_save.active = false; // one save per window
                b.pos = s.plunger.pos + s.plunger.lane_dir * (kBallRadius + 0.002f);
                b.vel = {0.0f, 0.0f};
                b.omega_z = 0.0f;
                b.last_safe_pos = b.pos;
            } else {
                b.live = false; // drained: slot freed, trough count grows
                ++s.trough_balls;
            }

            // §4.2 payload: balls still in play after this drain.
            int remaining = 0;
            for (const Ball& other : s.balls) {
                if (&other != &b && other.live && other.mode == BallMode::Free) {
                    ++remaining;
                }
            }

            emit_sound(s, int(SoundPurpose::Drain), nullptr, 8.0f);
            absorb(s, s.tick, SimEventType::Drain, 0xFFFE);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::Drain);
            ev.element = 0xFFFE;
            ev.x = b.pos.x;
            ev.y = b.pos.y;
            ev.a = float(remaining);
            ev.data = b.index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
            record_tick_event(s, ev);
        }
    }

    // ---- Trough serve (M5 basic loop: drain → auto-serve next ball).
    // When the M10 framework is attached it owns serving (11 §4.2:
    // BallReady/PlayerChange exit commands the eject; ball save and
    // add_ball serve with autolaunch) — this loop steps aside.
    if (s.fsm_step != nullptr) {
        s.serve_delay_ticks = 0;
    }
    bool any_free = false;
    for (const Ball& b : s.balls) {
        if (b.live && b.mode == BallMode::Free) {
            any_free = true;
            break;
        }
    }
    if (s.fsm_step != nullptr || any_free || s.trough_balls <= 0 || !s.has_plunger) {
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
                record_tick_event(s, ev);
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
            if (!s.coils_enabled) {
                break; // de-energized on tilt (11 §5): no impulse
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
            emit_sound(s, int(SoundPurpose::Slingshot), ball, c.approach);
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
            if (!s.coils_enabled) {
                break; // de-energized on tilt (11 §5): no impulse
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
            emit_sound(s, int(SoundPurpose::PopBumper), ball, length(ball->vel));
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
            emit_sound(s, int(SoundPurpose::StandupTarget), ball, c.approach);
            emit_element_event(s, SimEventType::SwitchHit, st.common.table_id, *ball, c.approach);
            break;
        }
    }

    // --- Drop banks (§6.5): facing-side contact >= 0.3 m/s while UP.
    for (DropBankElem& bank : s.drop_banks) {
        for (size_t ci = 0; ci < contact_log_n_; ++ci) {
            const LoggedContact& c = contact_log_[ci];
            if (c.element != bank.common.table_id || c.approach < 0.3f) {
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
            // Which UP target did this contact hit? The nearest face.
            size_t hit_idx = SIZE_MAX;
            float best_d = 1e9f;
            for (size_t ti = 0; ti < bank.targets.size(); ++ti) {
                const DropBankElem::Target& t = bank.targets[ti];
                if (t.state != DropTargetState::Up) {
                    continue;
                }
                const float d = point_segment_distance(ball->pos, t.face_a, t.face_b);
                if (d < best_d) {
                    const Vec2 rel = ball->pos - (t.face_a + t.face_b) * 0.5f;
                    if (dot(rel, t.face_normal) < 0.0f) {
                        continue; // back side never triggers
                    }
                    best_d = d;
                    hit_idx = ti;
                }
            }
            if (hit_idx == SIZE_MAX || best_d > kBallRadius + 0.004f) {
                continue;
            }
            DropBankElem::Target& t = bank.targets[hit_idx];
            t.state = DropTargetState::Dropping;
            t.anim_ticks = 120;
            emit_element_event(s, SimEventType::SwitchHit, bank.common.table_id, *ball, c.approach);
            emit_sound(s, int(SoundPurpose::DropTarget), ball, c.approach);
            absorb(s, s.tick, SimEventType::TargetDown, bank.common.table_id);
            SimEvent ev;
            ev.tick = s.tick;
            ev.type = uint16_t(SimEventType::TargetDown);
            ev.element = bank.common.table_id;
            ev.b = float(hit_idx + 1); // 1-based target_index (§6.5)
            ev.data = ball->index;
            s.render_ring.push(s.tick, ev);
            s.game_ring.push(s.tick, ev);
            record_tick_event(s, ev);
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
                    emit_sound(s, int(SoundPurpose::Rollover), &b, length(b.vel));
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
                // ONE revolution = ONE event set. The representative
                // ball is picked BEFORE the emissions so no reader can
                // mistake this for a per-ball loop (raised and rebutted
                // three review cycles running).
                const Ball* rep = nullptr;
                for (const Ball& b : s.balls) {
                    if (b.live) {
                        rep = &b;
                        break;
                    }
                }
                if (rep != nullptr) {
                    emit_sound(s, int(SoundPurpose::Spinner), rep, 8.0f);
                    emit_element_event(
                        s, SimEventType::SwitchHit, sp.common.table_id, *rep, 0.0f);
                    emit_element_event(s,
                                       SimEventType::SpinnerSpin,
                                       sp.common.table_id,
                                       *rep,
                                       std::abs(sp.plate_omega) * 60.0f / (2.0f * float(kPiF)));
                }
            }
        } else {
            sp.plate_omega = 0.0f;
        }
    }
}

// Eject the kicker's held ball at its element defaults (§6.9). Shared
// by the capture_ms failsafe, tb.kick, and the tilt force-eject.
void eject_kicker(SimState& s, KickerElem& k) {
    if (k.held_ball == 0xFF) {
        return;
    }
    emit_sound_x(s, int(SoundPurpose::Kicker), k.pos.x, k.eject_speed);
    for (Ball& b : s.balls) {
        if (b.live && b.index == k.held_ball) {
            const float phi = k.eject_angle_deg * float(3.14159265358979 / 180.0);
            b.pos = k.pos;
            b.vel = {k.eject_speed * float(std::cos(phi)), k.eject_speed * float(std::sin(phi))};
            b.omega_z = 0.0f;
            b.mode = BallMode::Free;
            b.last_safe_pos = b.pos;
            break;
        }
    }
    k.held_ball = 0xFF;
    k.has_hold = false;
    k.hold_ticks = 0;
}

void Solver::step_lifecycle(SimState& s, const TickInput* input) {
    (void)input;

    // --- Kicker dwell/eject (§6.9). ---
    for (KickerElem& k : s.kickers) {
        if (k.held_ball == 0xFF || !k.has_hold) {
            continue;
        }
        if (k.hold_ticks > 0) {
            --k.hold_ticks; // the capture_ms auto-eject failsafe
        }
        if (k.hold_ticks == 0) {
            Ball* ball = nullptr;
            for (Ball& b : s.balls) {
                if (b.live && b.index == k.held_ball) {
                    ball = &b;
                    break;
                }
            }
            if (ball == nullptr) {
                k.held_ball = 0xFF;
                k.has_hold = false;
                continue;
            }
            const float phi = k.eject_angle_deg * float(3.14159265358979 / 180.0);
            ball->pos = k.pos;
            ball->vel = {k.eject_speed * float(std::cos(phi)),
                         k.eject_speed * float(std::sin(phi))};
            ball->omega_z = 0.0f;
            ball->mode = BallMode::Free;
            ball->last_safe_pos = ball->pos;
            k.held_ball = 0xFF;
            k.has_hold = false;
            pushout(s); // immediately after eject (§6.9)
        }
    }

    // --- Drop banks (§6.5): drop/raise animations + bank complete +
    // auto reset. ---
    for (DropBankElem& bank : s.drop_banks) {
        for (DropBankElem::Target& t : bank.targets) {
            switch (t.state) {
            case DropTargetState::Up:
            case DropTargetState::Down:
                break;
            case DropTargetState::Dropping:
                if (t.anim_ticks > 0) {
                    --t.anim_ticks;
                }
                if (t.anim_ticks == 0) {
                    t.state = DropTargetState::Down;
                }
                break;
            case DropTargetState::Raising:
                if (t.anim_ticks > 0) {
                    --t.anim_ticks;
                }
                if (t.anim_ticks == 0) {
                    t.state = DropTargetState::Up;
                }
                break;
            }
        }
        // Bank completes when the last target settles DOWN (no target is
        // Up/Dropping/Raising) — checked once per tick, not per transition.
        if (bank.last_not_complete) {
            bool all_down = !bank.targets.empty();
            for (const DropBankElem::Target& t : bank.targets) {
                if (t.state != DropTargetState::Down) {
                    all_down = false;
                    break;
                }
            }
            if (all_down) {
                emit_bank_event(s, SimEventType::BankComplete, bank.common.table_id);
                bank.last_not_complete = false;
                if (bank.auto_reset) {
                    bank.reset_timer = bank.auto_reset_ticks;
                }
            }
        }
        if (bank.auto_reset && bank.reset_timer > 0) {
            --bank.reset_timer;
            if (bank.reset_timer == 0) {
                request_bank_reset(s, bank);
            }
        }
    }

    // --- Captive balls (§6.13): 1-D motion + end bounces + full travel. ---
    const float slope_rad = s.slope_deg * float(3.14159265358979 / 180.0);
    const float g_slope = kGravity * std::sin(slope_rad);
    const float rr = s.mu_rr * kGravity * std::cos(slope_rad);
    for (CaptiveBallElem& cap : s.captives) {
        // Gravity along the slot + rolling resistance.
        const float g_along = -g_slope * cap.axis.y;
        cap.s_dot += g_along * kTickDt;
        if (std::abs(cap.s_dot) > 0.0f) {
            const float drop = rr * kTickDt;
            if (drop >= std::abs(cap.s_dot)) {
                cap.s_dot = 0.0f;
            } else {
                cap.s_dot -= std::copysign(drop, cap.s_dot);
            }
        }
        cap.s_c += cap.s_dot * kTickDt;

        const float s_max = cap.slot_len - kBallRadius;
        if (cap.s_c >= s_max && cap.s_dot > 0.0f) {
            if (cap.s_dot >= 0.3f && cap.far_armed) {
                cap.far_armed = false; // hysteresis: re-arm 4 mm back
                for (Ball& b : s.balls) {
                    if (b.live) {
                        emit_element_event(s,
                                           SimEventType::CaptiveFullTravel,
                                           cap.common.table_id,
                                           b,
                                           std::abs(cap.s_dot));
                        break;
                    }
                }
            }
            cap.s_c = s_max;
            cap.s_dot = -0.4f * cap.s_dot; // far end bounce
        }
        const float s_min = kBallRadius;
        if (cap.s_c <= s_min && cap.s_dot < 0.0f) {
            cap.s_c = s_min;
            cap.s_dot = -0.4f * cap.s_dot; // near end bounce
        }
        if (!cap.far_armed && cap.s_c <= s_max - 0.004f) {
            cap.far_armed = true;
        }
    }

    // --- Ball locks (§6.14): capture region + failsafe + 500 ms eject. ---
    for (BallLockElem& lock : s.ball_locks) {
        if (s.coils_enabled && lock.held < lock.capacity) {
            for (Ball& b : s.balls) {
                if (!b.live || b.mode != BallMode::Free || b.layer != lock.common.layer) {
                    continue;
                }
                if (length(b.pos - lock.pos) >= 0.02f) {
                    continue;
                }
                // Capture is unconditional (§6.14).
                b.mode = BallMode::Captured;
                b.holder_elem = lock.common.table_id;
                b.hold_ticks = lock.claim_ticks;
                ++lock.held;
                ++s.locked_balls;
                emit_element_event(
                    s, SimEventType::SwitchHit, lock.common.table_id, b, length(b.vel));
                emit_lock_event(s, lock);
                break;
            }
        }

        // Unclaimed auto-release (§6.14): runs when the event was not
        // claimed by any handler; claim plumbing arrives with Lua (M9),
        // so until then locks never claim — the failsafe always applies.
        if (lock.claim_ticks > 0 && lock.held > 0 && lock.release_pending == 0) {
            // Oldest held ball's countdown lives on the CAPTURED ball.
            for (Ball& b : s.balls) {
                if (b.live && b.mode == BallMode::Captured &&
                    b.holder_elem == lock.common.table_id && b.hold_ticks > 0) {
                    --b.hold_ticks;
                    if (b.hold_ticks == 0) {
                        lock.release_pending = 1; // failsafe releases one
                    }
                    break;
                }
            }
        }

        if (lock.release_pending > 0) {
            if (lock.release_timer > 0) {
                --lock.release_timer;
            }
            if (lock.release_timer == 0) {
                // Eject exactly one ball (oldest first = lowest index).
                for (Ball& b : s.balls) {
                    if (b.live && b.mode == BallMode::Captured &&
                        b.holder_elem == lock.common.table_id) {
                        const float phi = lock.eject_angle_deg * float(3.14159265358979 / 180.0);
                        b.mode = BallMode::Free;
                        b.pos = lock.pos;
                        b.vel = {lock.eject_speed * float(std::cos(phi)),
                                 lock.eject_speed * float(std::sin(phi))};
                        b.omega_z = 0.0f;
                        b.last_safe_pos = b.pos;
                        --lock.held;
                        --s.locked_balls;
                        --lock.release_pending;
                        lock.release_timer = 500; // one per 500 ms (§6.14)
                        break;
                    }
                }
                if (lock.release_pending == 0) {
                    lock.release_timer = 0;
                }
            }
        }
    }

    // --- Ball save countdown. ---
    if (s.ball_save.active && s.ball_save.ticks_left > 0) {
        --s.ball_save.ticks_left;
        if (s.ball_save.ticks_left == 0) {
            s.ball_save.active = false;
        }
    }
}

void Solver::resolve_captive(SimState& s, Ball& ball, CaptiveBallElem& cap, Vec2 n) {
    // §6.13: the captive moves only along â. n̂ points captive → free ball
    // here (flip from the sweep convention, surface → ball center).
    const Vec2 n_hat = n; // sweep normal points from captive surface toward ball
    const Vec2 v_rel = ball.vel - cap.axis * cap.s_dot;
    const float u_n = dot(v_rel, n_hat);
    if (u_n >= 0.0f) {
        return; // separating
    }
    const float ca = dot(n_hat, cap.axis);
    if (std::abs(ca) < 1e-6f) {
        // Kinetic wall: resolve the free ball off an immovable captive.
        const float e = 0.9f;
        ball.vel -= n_hat * ((1.0f + e) * u_n);
        ball.vel = clamp_speed(ball.vel);
    } else {
        const float e = 0.9f;
        const float denom = 1.0f / kBallMass + (ca * ca) / kBallMass;
        const float j = -(1.0f + e) * u_n / denom;
        ball.vel += n_hat * (j / kBallMass);
        // Newton's third law on the captive: reaction −j·n̂, projected on
        // the slot axis â (n̂ points captive → ball in this convention).
        cap.s_dot -= (j * ca / kBallMass);
        ball.vel = clamp_speed(ball.vel);
    }

    // The strike's switch_hit (100 ms debounce per element, §6.13).
    if (cap.common.cooldown_left == 0) {
        cap.common.cooldown_left = cap.common.cooldown_ticks;
        emit_element_event(s, SimEventType::SwitchHit, cap.common.table_id, ball, length(ball.vel));
    }
}

void Solver::apply_script_actions(SimState& s, const std::vector<ScriptAction>& actions) {
    for (const ScriptAction& a : actions) {
        // Framework commands always apply (ForceEjectAll is itself the
        // tilt consequence).
        switch (a.kind) {
        case ScriptAction::Kind::FlippersEnabled:
            s.flippers_enabled = a.flag;
            continue;
        case ScriptAction::Kind::CoilsEnabled:
            s.coils_enabled = a.flag;
            if (!a.flag) {
                // §5: de-energized magnets are RELEASED, not merely
                // forbidden to re-energize (same as the tilt path).
                for (MagnetSim& mag : s.magnets) {
                    mag.set_active(false);
                }
            }
            continue;
        case ScriptAction::Kind::ResetDanger:
            reset_danger(s);
            continue;
        case ScriptAction::Kind::LocksToTrough: {
            // §4.5 step 5: whatever is still locked goes home as pure
            // bookkeeping — no eject, no kinematics, no drain event.
            for (BallLockElem& lock : s.ball_locks) {
                s.trough_balls += lock.held;
                s.locked_balls -= lock.held;
                lock.held = 0;
                lock.release_pending = 0;
                lock.release_timer = 0;
            }
            // Captured-by-kicker balls are NOT force-moved: capture_ms
            // keeps running (§5 binding) and ejects them on their own
            // timer.
            continue;
        }
        case ScriptAction::Kind::ForceEjectAll: {
            // 11-game-framework.md §5: every captured ball ejects at its
            // element defaults; locks empty one ball per 500 ms via the
            // ordinary release cadence; magnets released; script holds
            // (has_hold + its hold_ticks countdown) cleared —
            // capture_ticks itself is element config and stays.
            for (KickerElem& k : s.kickers) {
                eject_kicker(s, k);
            }
            for (BallLockElem& lock : s.ball_locks) {
                if (lock.held > 0) {
                    lock.release_pending = lock.held;
                }
            }
            for (MagnetSim& mag : s.magnets) {
                mag.set_active(false);
            }
            continue;
        }
        default:
            break;
        }
        // De-energized coils (11 §5): scripted kicks/holds/magnet calls
        // and lock releases are no-ops for the rest of a tilted ball.
        if (!s.coils_enabled) {
            switch (a.kind) {
            case ScriptAction::Kind::Kick:
            case ScriptAction::Kind::KickHold:
            case ScriptAction::Kind::ReleaseLock:
            case ScriptAction::Kind::MagnetOn:
            case ScriptAction::Kind::MagnetOff:
            case ScriptAction::Kind::MagnetPulse:
                continue;
            default:
                break;
            }
        }
        switch (a.kind) {
        case ScriptAction::Kind::Kick: {
            for (KickerElem& k : s.kickers) {
                if (k.common.table_id != a.element || k.held_ball == 0xFF) {
                    continue;
                }
                Ball* ball = nullptr;
                for (Ball& b : s.balls) {
                    if (b.live && b.index == k.held_ball) {
                        ball = &b;
                        break;
                    }
                }
                if (ball == nullptr) {
                    break;
                }
                const float speed = a.use_speed ? a.speed : k.eject_speed;
                const float phi = (a.use_angle ? a.angle_deg : k.eject_angle_deg) *
                                  float(3.14159265358979 / 180.0);
                ball->pos = k.pos;
                ball->vel = {speed * float(std::cos(phi)), speed * float(std::sin(phi))};
                ball->omega_z = 0.0f;
                ball->mode = BallMode::Free;
                ball->last_safe_pos = ball->pos;
                k.held_ball = 0xFF;
                k.has_hold = false;
                k.hold_ticks = 0;
            }
            break;
        }
        case ScriptAction::Kind::KickHold:
            for (KickerElem& k : s.kickers) {
                if (k.common.table_id == a.element && k.held_ball != 0xFF) {
                    k.has_hold = true;
                    k.hold_ticks = 0; // §6.9 script-held: auto-eject off
                }
            }
            break;
        case ScriptAction::Kind::ReleaseLock:
            for (BallLockElem& lock : s.ball_locks) {
                if (lock.common.table_id == a.element) {
                    lock.release_pending += a.count;
                }
            }
            break;
        case ScriptAction::Kind::MagnetOn:
        case ScriptAction::Kind::MagnetOff:
        case ScriptAction::Kind::MagnetPulse:
            for (MagnetSim& mag : s.magnets) {
                if (mag.table_id != a.element) {
                    continue;
                }
                if (a.kind == ScriptAction::Kind::MagnetOn) {
                    mag.set_active(true);
                    mag.hum_tick = uint32_t(s.tick);
                    emit_sound_x(s, int(SoundPurpose::Magnet), mag.pos.x, 8.0f); // §7.2
                } else if (a.kind == ScriptAction::Kind::MagnetOff) {
                    mag.set_active(false);
                } else {
                    mag.pulse(uint32_t(a.speed));
                    mag.hum_tick = uint32_t(s.tick);
                    emit_sound_x(s, int(SoundPurpose::Magnet), mag.pos.x, 8.0f);
                }
            }
            break;
        case ScriptAction::Kind::SetFlipperEnabled:
            for (Flipper& f : s.flippers) {
                if (f.table_id == a.element) {
                    f.enabled = a.flag;
                }
            }
            break;
        case ScriptAction::Kind::AddBall:
            for (int i = 0; i < a.count && s.trough_balls > 0; ++i) {
                serve_ball_notified(s); // §4.2: BallServed closes the
                                        // framework's serve window
            }
            break;
        case ScriptAction::Kind::DropBankReset:
            for (DropBankElem& bank : s.drop_banks) {
                if (bank.common.table_id == a.element) {
                    request_bank_reset(s, bank);
                }
            }
            break;
        case ScriptAction::Kind::GateOpen:
        case ScriptAction::Kind::GateClose:
            for (GateElem& g : s.gates) {
                if (g.common.table_id != a.element || g.mechanical) {
                    continue; // §6.7: one-way gates ignore script control
                }
                g.state =
                    a.kind == ScriptAction::Kind::GateOpen ? GateState::Open : GateState::Closed;
            }
            break;
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
                if (!collider_enabled(s, ci)) {
                    continue; // dropped target (§6.5) — no depenetration
                }
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
                case Collider::Kind::Arc: {
                    // Distance to the circle surface, gated to the arc's
                    // angular span (endpoint caps own the tips).
                    const Vec2 d = ball.pos - c.a;
                    const float dist = length(d);
                    const float phi = wrap_ccw(std::atan2(d.y, d.x) - c.a0);
                    if (phi > wrap_ccw(c.a1 - c.a0)) {
                        break;
                    }
                    sep = std::fabs(dist - c.radius) - kBallRadius;
                    const Vec2 radial = dist > 1e-9f ? d * (1.0f / dist) : Vec2{0.0f, 1.0f};
                    n = dist > c.radius ? radial : radial * -1.0f;
                    break;
                }
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

void absorb_framework_event(SimState& s, const char* name) {
    // Same 12-byte layout as absorb() with type = 0xF000.. so framework
    // and sim events never collide in the accumulator.
    const uint64_t tick = s.tick;
    const uint16_t type = 0xF000u;
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
        u8(type, 0),
        u8(type, 8),
        0xFF,
        0xFF,
    };
    uint64_t h = fnv1a64(bytes, sizeof(bytes), s.event_seq_hash);
    h = fnv1a64(name, std::strlen(name), h);
    s.event_seq_hash = h;
}

// 08 §7.3: reset all danger state — bob p/v, abuse accumulator, arm
// latches, crossing counter, and in-flight nudge envelopes. The input
// edge latch (input_prev_buttons) is input plumbing, not danger state:
// the button stream is continuous across balls and its edges must not
// be re-fired. Commanded by the framework at end of ball
// (11-game-framework.md §4.5 step 5); never a timer.
void reset_danger(SimState& s) {
    // 08 §7.3: the framework commands this at every end of ball;
    // danger is strictly per ball. The physics.tilt thresholds are
    // TABLE tuning (09 §2), not per-ball state — preserve them.
    const float warn = s.tilt.warn_m, hard = s.tilt.hard_m, abuse = s.tilt.abuse_mps;
    s.tilt = SimState::TiltState{};
    s.tilt.warn_m = warn;
    s.tilt.hard_m = hard;
    s.tilt.abuse_mps = abuse;
    for (SimState::NudgeEnvelope& e : s.nudge_envelopes) {
        e = SimState::NudgeEnvelope{};
    }
}

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

    // Tilt bob + abuse accumulator are replayed state (08 §7.2/§7.3:
    // nudges are inputs), as are the active nudge envelopes.
    mix(&s.tilt.p.x, 4);
    mix(&s.tilt.p.y, 4);
    mix(&s.tilt.v.x, 4);
    mix(&s.tilt.v.y, 4);
    mix(&s.tilt.abuse_acc, 4);
    mix(&s.tilt.crossings, sizeof(s.tilt.crossings));
    // Arm latches and the framework gates gate future emission and
    // physics — genuine replayed state.
    const auto u8c = [](uint64_t v, int shift) {
        return static_cast<unsigned char>((v >> shift) & 0xFFu);
    };
    const uint64_t armed = (s.tilt.warn_armed ? 1u : 0u) | (s.tilt.hard_armed ? 2u : 0u) |
                           (s.tilt.abuse_armed ? 4u : 0u);
    const uint64_t gates = (s.flippers_enabled ? 1u : 0u) | (s.coils_enabled ? 2u : 0u);
    const unsigned char bytes[2] = {u8c(armed, 0), u8c(gates, 0)};
    mix(bytes, sizeof(bytes));
    for (const SimState::NudgeEnvelope& e : s.nudge_envelopes) {
        mix(&e.ax, 4);
        mix(&e.ay, 4);
        mix(&e.ticks_left, sizeof(e.ticks_left));
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
