#include "sim/ccd.h"

#include <cmath>

namespace tb::sim {

namespace {

constexpr float kPi = 3.14159265358979f;

float wrap_ccw(float a) {
    // Map any angle to [0, 2π).
    float two_pi = 2.0f * kPi;
    float r = std::fmod(a, two_pi);
    if (r < 0.0f) {
        r += two_pi;
    }
    return r;
}

} // namespace

bool sweep_circle_vs_segment(Vec2 p0, Vec2 v, float r, Vec2 a, Vec2 b, float max_t, SweepHit& out) {
    const Vec2 d = b - a;
    const float len = length(d);
    if (len <= 0.0f) {
        return false;
    }
    const Vec2 dhat = d * (1.0f / len);
    const Vec2 n = perp(dhat);

    const float sn0 = dot(p0 - a, n);
    const float vn = dot(v, n);
    const float sigma = (sn0 >= 0.0f) ? 1.0f : -1.0f;

    SweepHit best;
    bool found = false;

    // Interior slab hit.
    if (std::fabs(sn0) >= r) {
        if (vn * sigma < 0.0f) {
            const float t_hit = (sigma * r - sn0) / vn;
            if (t_hit >= 0.0f - kToiEps && t_hit <= max_t) {
                const Vec2 q = p0 + v * t_hit;
                const float u = dot(q - a, dhat);
                if (u >= 0.0f && u <= len) {
                    best.toi = t_hit;
                    best.normal = n * sigma;
                    found = true;
                }
            }
        }
    } else if (vn * sigma < 0.0f) {
        // Inside the slab and approaching: immediate contact.
        best.toi = 0.0f;
        best.normal = n * sigma;
        found = true;
    }

    // Endpoint caps (§3.2 fall-through): swept point tests on A and B.
    // The earliest hit wins.
    SweepHit cap;
    if (sweep_circle_vs_point(p0, v, r, a, r, max_t, cap)) {
        if (!found || cap.toi < best.toi) {
            best = cap;
            found = true;
        }
    }
    if (sweep_circle_vs_point(p0, v, r, b, r, max_t, cap)) {
        if (!found || cap.toi < best.toi) {
            best = cap;
            found = true;
        }
    }

    if (found) {
        out = best;
    }
    return found;
}

bool sweep_circle_vs_point(
    Vec2 p0, Vec2 v, float r, Vec2 c, float rho, float max_t, SweepHit& out) {
    // Root-finding in double: the §2.1 worked-example tolerances assume
    // exact-decimal geometry, which single-precision cancellation cannot
    // reproduce near-tangent impacts.
    const double wx = double(p0.x) - double(c.x);
    const double wy = double(p0.y) - double(c.y);
    const double vx = double(v.x);
    const double vy = double(v.y);
    const double rr = double(rho);

    const double a = vx * vx + vy * vy;
    const double b = 2.0 * (wx * vx + wy * vy);
    const double cc = wx * wx + wy * wy - rr * rr;

    if (cc < 0.0) {
        // Already overlapping.
        if (b < 0.0) {
            const double dl = std::sqrt(wx * wx + wy * wy);
            out.toi = 0.0f;
            out.normal = {float(wx / dl), float(wy / dl)};
            return true;
        }
        return false;
    }
    if (b >= 0.0) {
        return false; // moving away
    }

    const double disc = b * b - 4.0 * a * cc;
    if (disc < 0.0) {
        return false;
    }
    const double t_hit = (-b - std::sqrt(disc)) / (2.0 * a);
    if (t_hit < 0.0 - double(kToiEps) || t_hit > double(max_t)) {
        return false;
    }

    const double qx = double(p0.x) + vx * t_hit - double(c.x);
    const double qy = double(p0.y) + vy * t_hit - double(c.y);
    const double ql = std::sqrt(qx * qx + qy * qy);
    out.toi = float(t_hit);
    out.normal = {float(qx / ql), float(qy / ql)};
    return true;
}

bool sweep_circle_vs_arc(Vec2 p0,
                         Vec2 v,
                         float r,
                         Vec2 center,
                         float radius,
                         float a0,
                         float a1,
                         float max_t,
                         SweepHit& out) {
    const Vec2 w = p0 - center;
    const float w_sq = dot(w, w);
    const float a = dot(v, v);
    const float b = 2.0f * dot(w, v);

    struct Surface {
        float rho;
        bool inner;
    };

    const Surface surfaces[] = {
        {radius + r, false}, // outer
        {radius - r, true},  // inner (requires R > r)
    };

    for (const Surface& s : surfaces) {
        if (s.inner && radius <= r) {
            continue;
        }
        const float cc = w_sq - s.rho * s.rho;

        // Outer hits require starting outside; inner hits require starting
        // inside. Boundary-equal starts are covered by push-out.
        const bool started_outside = w_sq > s.rho * s.rho;
        if (s.inner == started_outside) {
            continue;
        }

        float disc = b * b - 4.0f * a * cc;
        if (disc < 0.0f) {
            continue;
        }
        disc = std::sqrt(disc);

        if (!(b < 0.0f)) {
            continue; // not approaching
        }

        float t_hit;
        if (s.inner) {
            t_hit = (-b + disc) / (2.0f * a); // positive root
        } else {
            t_hit = (-b - disc) / (2.0f * a);
        }
        if (t_hit < 0.0f - kToiEps || t_hit > max_t) {
            continue;
        }

        const Vec2 q = p0 + v * t_hit;
        const float phi = wrap_ccw(std::atan2((q - center).y, (q - center).x) - a0);
        const float span = wrap_ccw(a1 - a0);
        if (phi > span) {
            continue; // outside the angular window
        }

        out.toi = t_hit;
        out.normal = s.inner ? normalize(center - q) : normalize(q - center);
        return true;
    }
    return false;
}

bool sweep_circle_vs_circle(
    Vec2 p0, Vec2 v0, float r0, Vec2 p1, Vec2 v1, float r1, float max_t, SweepHit& out) {
    // Relative motion; combined radius. Normal points from body 1 toward
    // the contact (i.e., toward body 2's center at TOI).
    const Vec2 rel_p = p1 - p0;
    const Vec2 rel_v = v1 - v0;
    const float rho = r0 + r1;

    const float a = dot(rel_v, rel_v);
    const float b = 2.0f * dot(rel_p, rel_v);
    const float c = dot(rel_p, rel_p) - rho * rho;

    if (c < 0.0f) {
        // Overlapping already — resolution handles it via push-out; no TOI.
        return false;
    }
    if (b >= 0.0f) {
        return false; // separating
    }

    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) {
        return false;
    }
    const float t_hit = (-b - std::sqrt(disc)) / (2.0f * a);
    if (t_hit < 0.0f - kToiEps || t_hit > max_t) {
        return false;
    }

    out.toi = t_hit;
    out.normal = normalize(rel_p + rel_v * t_hit); // toward body 2
    return true;
}

} // namespace tb::sim
