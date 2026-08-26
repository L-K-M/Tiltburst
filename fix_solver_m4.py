p = "src/sim/solver.cpp"
s = open(p, encoding="utf-8").read()

# 1) Generalized resolve with surface velocity + live catch support
old = """float Solver::resolve_static(SimState& s, Ball& ball, Vec2 n, const Material& mat) {
    // \u00a74.1 impulse equations; surface velocity zero for static colliders.
    const Vec2 t = perp(n);

    // Ball surface-point velocity: v_p = v \u2212 r\u00b7omega_z\u00b7perp(n\u0302); u = v_p.
    const Vec2 vp = ball.vel - kBallRadius * ball.omega_z * perp(n);
    const float u_n = dot(vp, n);
    if (u_n >= 0.0f) {
        return 0.0f; // separating \u2014 never apply impulses
    }
    const float u_t = dot(vp, t);

    const float approach = -u_n;
    const float e_eff = restitution_curve(s, mat.restitution, approach);
    const float j_n = -(1.0f + e_eff) * u_n * kBallMass;"""
new = """float Solver::resolve_surface(SimState& s, Ball& ball, Vec2 n,
                              const Material& mat, Vec2 surface_vel,
                              float live_catch_scale) {
    // \u00a74.1 impulse equations against an infinite-mass moving surface.
    // V_s is zero for static colliders; \u00a73.5 for flippers.
    const Vec2 t = perp(n);

    // Relative contact velocity: u = v_p \u2212 V_s.
    const Vec2 vp = ball.vel - kBallRadius * ball.omega_z * perp(n);
    const Vec2 u_vec = vp - surface_vel;
    const float u_n = dot(u_vec, n);
    if (u_n >= 0.0f) {
        return 0.0f; // separating \u2014 never apply impulses
    }
    const float u_t = dot(u_vec, t);

    const float approach = -u_n;
    float e_eff =
        restitution_curve(s, mat.restitution, approach) * live_catch_scale;
    float mu_s = mat.mu_s;
    float mu_k = mat.mu_k;
    if (live_catch_scale < 1.0f) {
        mu_s = std::min(1.0f, 2.0f * mu_s); // tangential grip (\u00a75.4)
        mu_k = std::min(1.0f, 2.0f * mu_k);
    }
    const float j_n = -(1.0f + e_eff) * u_n * kBallMass;"""
assert old in s, "resolve head"
s = s.replace(old, new)

old_jt = """    const float j_stick = -(2.0f * kBallMass / 7.0f) * u_t;
    float j_t;
    if (std::fabs(j_stick) <= mat.mu_s * j_n) {
        j_t = j_stick;
    } else {
        j_t = -std::copysign(mat.mu_k * j_n, u_t);
    }

    ball.vel += (j_n * n + j_t * t) * (1.0f / kBallMass);"""
new_jt = """    const float j_stick = -(2.0f * kBallMass / 7.0f) * u_t;
    float j_t;
    if (std::fabs(j_stick) <= mu_s * j_n) {
        j_t = j_stick;
    } else {
        j_t = -std::copysign(mu_k * j_n, u_t);
    }

    ball.vel += (j_n * n + j_t * t) * (1.0f / kBallMass);"""
assert old_jt in s, "jt"
s = s.replace(old_jt, new_jt)

# rename remaining resolve_static uses
s = s.replace("void Solver::pushout", "void Solver::pushout")

# header decl update
open(p, "w", encoding="utf-8").write(s)

h = "src/sim/solver.h"
t = open(h, encoding="utf-8").read()
old_h = """    Contact find_earliest(SimState& s, float t_cur);
    float resolve_static(SimState& s, Ball& ball, Vec2 normal,
                         const Material& mat); // returns approach speed"""
new_h = """    Contact find_earliest(SimState& s, float t_cur);
    float resolve_surface(SimState& s, Ball& ball, Vec2 normal,
                          const Material& mat, Vec2 surface_vel,
                          float live_catch_scale); // returns approach speed
    void resolve_flipper(SimState& s, Ball& ball, Flipper& f,
                         const FlipperHit& hit);"""
assert old_h in t, "header"
t = t.replace(old_h, new_h)
t = t.replace('#include "sim/flipper.h"', '#include "sim/flipper.h"\n#include "sim/flipper_ccd.h"')
open(h, "w", encoding="utf-8").write(t)
print("ok")
