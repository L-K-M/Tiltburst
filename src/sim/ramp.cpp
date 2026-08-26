#include "sim/ramp.h"

#include <algorithm>
#include <cmath>

namespace tb::sim {

namespace {

Vec2 lerp2(Vec2 a, Vec2 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

const RampPath::Sample& sample_at(const std::vector<RampPath::Sample>& samples, float s) {
    // Binary search for the bracketing sample; callers clamp s to [0, S].
    size_t lo = 0;
    size_t hi = samples.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (samples[mid].s <= s) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return samples[lo];
}

} // namespace

Vec2 RampPath::point_at(float s) const {
    const float clamped = std::clamp(s, 0.0f, total_s);
    const Sample& a = sample_at(samples, clamped);
    const Sample& b = samples[std::min(size_t(&a - samples.data() + 1), samples.size() - 1)];
    const float span = std::max(b.s - a.s, 1e-9f);
    return lerp2(a.pos, b.pos, (clamped - a.s) / span);
}

Vec2 RampPath::tangent_at(float s) const {
    const float clamped = std::clamp(s, 0.0f, total_s);
    const Sample& a = sample_at(samples, clamped);
    const Sample& b = samples[std::min(size_t(&a - samples.data() + 1), samples.size() - 1)];
    const Vec2 d = b.pos - a.pos;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    return len > 1e-9f ? Vec2{d.x / len, d.y / len} : a.tangent;
}

float RampPath::z_at(float s) const {
    const float clamped = std::clamp(s, 0.0f, total_s);
    const Sample& a = sample_at(samples, clamped);
    const Sample& b = samples[std::min(size_t(&a - samples.data() + 1), samples.size() - 1)];
    const float span = std::max(b.s - a.s, 1e-9f);
    return a.z + (b.z - a.z) * ((clamped - a.s) / span);
}

Vec2 MagnetSim::accel(const Ball& ball) const {
    if (!on) {
        return {0.0f, 0.0f};
    }
    const float d_true = length(ball.pos - pos);
    if (d_true > radius) {
        return {0.0f, 0.0f};
    }
    const float d = std::max(d_true, 0.010f);       // core clamp (§6.12)
    float f = strength * (0.05f / d) * (0.05f / d); // F = strength·(0.05/d)²
    const float fade = std::clamp((radius - d_true) / 0.01f, 0.0f, 1.0f);
    float pulse_scale = 1.0f;
    if (pulse_total > 0) {
        // Pulse envelope: full for 0.6·T, linear decay to 0 over 0.4·T.
        const float done = 1.0f - float(pulse_ticks_left) / float(pulse_total);
        pulse_scale = done <= 0.6f ? 1.0f : std::max(0.0f, 1.0f - (done - 0.6f) / 0.4f);
    }
    const float a_mag = std::min(f / kBallMass, 60.0f) * fade * pulse_scale;
    const Vec2 to_pos = d_true > 1e-9f ? (pos - ball.pos) * (1.0f / d_true) : Vec2{0.0f, 1.0f};
    return to_pos * a_mag;
}

void MagnetSim::damp(Ball& ball) const {
    if (!on) {
        return;
    }
    if (length(ball.pos - pos) > radius) {
        return;
    }
    // Eddy damping (§6.12, ADR-023): spin dies fast; the velocity brake
    // at 3.5/s is what lets the field actually hold a through ball — at
    // the spec's original 0.8/s every transit escaped with the gravity
    // gain (0.4 m²/s² over a rim-to-rim pass) intact.
    ball.omega_z *= std::exp(-8.0f * kTickDt);
    const float vel_scale = std::exp(-3.5f * kTickDt);
    ball.vel = ball.vel * vel_scale;
}

} // namespace tb::sim
