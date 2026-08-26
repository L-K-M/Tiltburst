#include "sim/flipper_ccd.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace tb::sim {

namespace {

struct LocalHit {
    float d;
    Vec2 n_local;
};

// sd_flipper plus the branch normal, in the local frame (§3.5).
LocalHit sd_flipper(Vec2 p, const Flipper& f) {
    const float L = f.params.length;
    const float rb = f.params.radius_base;
    const float rt = f.params.radius_tip;
    const float k = (rb - rt) / L;
    const float a = std::sqrt(1.0f - k * k);
    const float qc = std::fabs(p.y);
    const float ql = p.x;
    const float m = -k * qc + a * ql;

    LocalHit hit{};
    if (m < 0.0f) {
        const float len = length(p);
        hit.d = len - rb;
        hit.n_local = len > 1e-9f ? p * (1.0f / len) : Vec2{1.0f, 0.0f};
    } else if (m > a * L) {
        const Vec2 tip = p - Vec2{L, 0.0f};
        const float len = length(tip);
        hit.d = len - rt;
        hit.n_local = len > 1e-9f ? tip * (1.0f / len) : Vec2{1.0f, 0.0f};
    } else {
        hit.d = a * qc + k * ql - rb;
        hit.n_local = {k, a * ((p.y >= 0.0f) ? 1.0f : -1.0f)};
        // Defensive normalize (k,a is already unit for |k|≤1).
        const float nl = length(hit.n_local);
        if (nl > 1e-9f) {
            hit.n_local = hit.n_local * (1.0f / nl);
        }
    }
    return hit;
}

Vec2 to_local(Vec2 world, const Flipper& f, float theta) {
    const Vec2 rel = world - f.params.pivot;
    const float c = std::cos(theta), sn = std::sin(theta);
    return {rel.x * c + rel.y * sn, -rel.x * sn + rel.y * c}; // R(−θ)
}

Vec2 to_world_dir(Vec2 local_dir, float theta) {
    const float c = std::cos(theta), sn = std::sin(theta);
    return {local_dir.x * c - local_dir.y * sn, local_dir.x * sn + local_dir.y * c}; // R(+θ)
}

void finish_hit(Vec2 pc,
                float r,
                const Flipper& f,
                float theta,
                float omega,
                float t,
                const LocalHit& lh,
                FlipperHit& out) {
    out.toi = t;
    out.normal = normalize(to_world_dir(lh.n_local, theta));
    out.contact = pc - out.normal * r;
    out.surface_vel = omega * perp(out.contact - f.params.pivot);
}

// Static fast path at θ0: swept tests against the two caps and the two
// tangent side segments (§3.5 relative-velocity path).
bool sweep_static_capsule(Vec2 p0,
                          Vec2 v,
                          float r,
                          const Flipper& f,
                          float theta0,
                          float omega,
                          float max_t,
                          FlipperHit& out) {
    const float L = f.params.length;
    const Vec2 axis{std::cos(theta0), std::sin(theta0)};
    const Vec2 pivot = f.params.pivot;
    const Vec2 base_c = pivot;
    const Vec2 tip_c = pivot + axis * L;

    SweepHit best;
    bool found = false;

    auto consider_point = [&](Vec2 center, float post_radius) {
        SweepHit h;
        if (sweep_circle_vs_point(p0, v, r, center, post_radius, max_t, h)) {
            if (!found || h.toi < best.toi) {
                best = h;
                found = true;
            }
        }
    };
    auto consider_segment = [&](Vec2 a, Vec2 b) {
        SweepHit h;
        if (sweep_circle_vs_segment(p0, v, r, a, b, max_t, h)) {
            if (!found || h.toi < best.toi) {
                best = h;
                found = true;
            }
        }
    };

    consider_point(base_c, f.params.radius_base + r);
    consider_point(tip_c, f.params.radius_tip + r);

    // Tangent side segments: offset from the axis by a_t·r_b at the base
    // and a_t·r_t at the tip, where a_t = sqrt(1 − k²).
    const float k = (f.params.radius_base - f.params.radius_tip) / L;
    const float a_t = std::sqrt(std::max(0.0f, 1.0f - k * k));
    const Vec2 n_up = perp(axis);

    for (int side : {-1, 1}) {
        const Vec2 pb = base_c + n_up * side * (a_t * f.params.radius_base);
        const Vec2 pt = tip_c + n_up * side * (a_t * f.params.radius_tip);
        consider_segment(pb, pt);
    }

    if (!found) {
        return false;
    }
    out.toi = best.toi;
    out.normal = best.normal;
    out.contact = p0 + v * best.toi - best.normal * r;
    out.surface_vel = omega * perp(out.contact - pivot); // ω ≈ 0 here anyway
    return true;
}

} // namespace

FlipperSep flipper_separation(Vec2 p, float r, const Flipper& f, float theta) {
    const LocalHit lh = sd_flipper(to_local(p, f, theta), f);
    FlipperSep out;
    out.sep = lh.d - r;
    out.normal = normalize(to_world_dir(lh.n_local, theta));
    const Vec2 contact = p - out.normal * r;
    out.surface_vel = f.omega * perp(contact - f.params.pivot);
    return out;
}

bool sweep_circle_vs_flipper(
    Vec2 p0, Vec2 v, float r, const Flipper& f, float max_t, FlipperHit& out) {
    const float theta0 = f.theta_start;
    const float omega = f.omega;

    if (std::fabs(omega) < kMovingOmegaFastPath) {
        return sweep_static_capsule(p0, v, r, f, theta0, omega, max_t, out);
    }

    // Conservative advancement (§3.5).
    const float bound =
        length(v) +
        std::fabs(omega) * (f.params.length + std::max(f.params.radius_base, f.params.radius_tip));
    if (!(bound > 0.0f)) {
        return false;
    }

    float t = 0.0f;
    LocalHit lh{};
    for (int iter = 0; iter < 24; ++iter) {
        const Vec2 pc = p0 + v * t;
        lh = sd_flipper(to_local(pc, f, theta0 + omega * t), f);
        const float d = lh.d - r;
        if (d < kSkin) {
            finish_hit(pc, r, f, theta0 + omega * t, omega, t, lh, out);
            return true;
        }
        t += std::max(d / bound, 1e-6f);
        if (t > max_t) {
            return false;
        }
    }

    // Converged tight: treat as hit.
    const Vec2 pc = p0 + v * t;
    lh = sd_flipper(to_local(pc, f, theta0 + omega * t), f);
    if (t <= max_t) {
        finish_hit(pc, r, f, theta0 + omega * t, omega, t, lh, out);
        return true;
    }
    return false;
}

} // namespace tb::sim
