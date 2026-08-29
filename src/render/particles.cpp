#include "render/particles.h"

#include "core/time.h"

#include <algorithm>
#include <cmath>

namespace tb::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

uint32_t lerp_rgba(uint32_t a, uint32_t b, float t) {
    const auto ch = [a, b, t](int shift) {
        const float ca = float((a >> shift) & 0xFFu);
        const float cb = float((b >> shift) & 0xFFu);
        return uint32_t(ca + (cb - ca) * t);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

uint32_t scale_rgba(uint32_t c, float f) {
    const auto ch = [c, f](int shift) { return uint32_t(float((c >> shift) & 0xFFu) * f); };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

} // namespace

uint64_t particle_seed_ns(); // below

ParticleSystem::ParticleSystem() {
    // Wall-clock seed (§13.1), never tb.rng.
    rng_state_ = particle_seed_ns() ^ 0x9E3779B97F4A7C15ull;
    if (rng_state_ == 0) {
        rng_state_ = 1;
    }
}

uint32_t ParticleSystem::rng_u32() {
    // xorshift64* — render-owned, wall-seeded (§13.1: not tb.rng).
    rng_state_ ^= rng_state_ >> 12;
    rng_state_ ^= rng_state_ << 25;
    rng_state_ ^= rng_state_ >> 27;
    return uint32_t((rng_state_ * 0x2545F4914F6CDD1Dull) >> 32);
}

float ParticleSystem::rng_range(float lo, float hi) {
    return lo + (hi - lo) * (float(rng_u32() >> 8) * 0x1.0p-24f);
}

void ParticleSystem::set_palette(const uint32_t (&roles)[8]) {
    for (int i = 0; i < 8; ++i) {
        palette_[size_t(i)] = roles[size_t(i)];
    }
}

int ParticleSystem::spawn(Effect effect, float x, float y, const float* dir) {
    // §13.4 canonical rows (binding). Role indices follow palette order.
    const int PRIMARY = 2, SECONDARY = 3, ACCENT1 = 4, WARM = 6, BG1 = 1, GLOW = 7, ACCENT2 = 5;
    Emitter e;
    switch (effect) {
    case Effect::BumperHitBurst:
        e.burst = 24;
        e.shape = 1;
        e.ring_r = 0.02f;
        e.speed_min = 0.8f;
        e.speed_max = 1.6f;
        e.life_min = 0.25f;
        e.life_max = 0.45f;
        e.size0 = 0.004f;
        e.size1 = 0.001f;
        e.color0 = palette_[PRIMARY];
        e.color1 = palette_[ACCENT1];
        e.spark = true;
        e.drag = 3.0f;
        break;
    case Effect::SlingFlash:
        e.burst = 12;
        e.shape = 2;
        if (dir != nullptr) {
            e.cone_dir[0] = dir[0];
            e.cone_dir[1] = dir[1];
        }
        e.cone_spread_deg = 60.0f;
        e.speed_min = 1.0f;
        e.speed_max = 2.0f;
        e.life_min = 0.20f;
        e.life_max = 0.35f;
        e.size0 = 0.005f;
        e.size1 = 0.0015f;
        e.color0 = palette_[WARM];
        e.color1 = palette_[PRIMARY];
        e.drag = 2.0f;
        break;
    case Effect::RampTrail:
        e.rate_mode = true;
        e.rate = 120.0;
        e.speed_min = 0.1f;
        e.speed_max = 0.3f;
        e.life_min = 0.30f;
        e.life_max = 0.60f;
        e.size0 = 0.003f;
        e.size1 = 0.0005f;
        e.color0 = palette_[SECONDARY];
        e.color1 = scale_rgba(palette_[SECONDARY], 0.0f);
        e.drag = 4.0f;
        break;
    case Effect::BallTrail:
        e.rate_mode = true;
        e.rate = 90.0;
        e.speed_min = 0.05f;
        e.speed_max = 0.15f;
        e.life_min = 0.15f;
        e.life_max = 0.30f;
        e.size0 = 0.005f;
        e.size1 = 0.001f;
        e.color0 = scale_rgba(palette_[GLOW], 0.35f);
        e.color1 = scale_rgba(palette_[GLOW], 0.0f);
        e.drag = 6.0f;
        break;
    case Effect::DrainBurst:
        e.burst = 40;
        e.shape = 2;
        e.cone_dir[0] = 0.0f;
        e.cone_dir[1] = 1.0f; // up-table
        e.cone_spread_deg = 40.0f;
        e.speed_min = 0.5f;
        e.speed_max = 1.5f;
        e.life_min = 0.40f;
        e.life_max = 0.80f;
        e.size0 = 0.004f;
        e.size1 = 0.001f;
        e.color0 = palette_[WARM];
        e.color1 = palette_[BG1];
        e.spark = true;
        e.grav = 3.0f;
        e.drag = 1.0f;
        break;
    case Effect::JackpotStarburst:
        e.burst = 96;
        e.shape = 1;
        e.ring_r = 0.01f;
        e.speed_min = 1.5f;
        e.speed_max = 3.0f;
        e.life_min = 0.60f;
        e.life_max = 1.00f;
        e.size0 = 0.006f;
        e.size1 = 0.001f;
        e.color0 = palette_[GLOW];
        e.color1 = palette_[ACCENT2];
        e.spark = true;
        e.drag = 2.5f;
        if (flash_reduction_) {
            e.burst /= 2;
        }
        break;
    case Effect::MagnetSparks:
        e.rate_mode = true;
        e.rate = 200.0;
        e.shape = 1;
        e.ring_r = 0.02f;
        e.speed_min = 0.3f;
        e.speed_max = 0.8f;
        e.life_min = 0.10f;
        e.life_max = 0.25f;
        e.size0 = 0.002f;
        e.size1 = 0.0005f;
        e.color0 = palette_[ACCENT1];
        e.color1 = scale_rgba(palette_[ACCENT1], 0.0f);
        e.spark = true;
        e.drag = 0.5f;
        break;
    case Effect::TiltWarningFlash:
        e.burst = 30;
        e.shape = 1;
        e.ring_r = 0.03f;
        e.speed_min = 0.4f;
        e.speed_max = 1.0f;
        e.life_min = 0.30f;
        e.life_max = 0.50f;
        e.size0 = 0.005f;
        e.size1 = 0.001f;
        e.color0 = palette_[WARM];
        e.color1 = scale_rgba(palette_[WARM], 0.0f);
        e.drag = 2.0f;
        break;
    default:
        return 0;
    }

    const int before = int(live_);
    if (e.rate_mode) {
        // Rate effects accumulate in update() via their callers feeding
        // positions each frame; a direct spawn() call is a single
        // particle (scripted EffectRequests use burst effects).
        emit(e, x, y, dir);
    } else {
        for (int i = 0; i < e.burst; ++i) {
            emit(e, x, y, dir);
        }
    }
    return int(live_) - before;
}

void ParticleSystem::emit(const Emitter& e, float x, float y, const float* dir) {
    // Steal the oldest when full (§13.1).
    uint32_t slot;
    if (live_ < kMaxParticles) {
        slot = live_++;
    } else {
        slot = oldest_ % kMaxParticles;
        oldest_ = (oldest_ + 1) % kMaxParticles;
        ++stolen_total_;
    }

    // Direction by shape.
    float dx = 0.0f, dy = 0.0f;
    const float speed = rng_range(e.speed_min, e.speed_max);
    if (e.shape == 0) {
        dx = rng_range(-1.0f, 1.0f);
        dy = rng_range(-1.0f, 1.0f);
        const float n = std::sqrt(dx * dx + dy * dy);
        if (n > 1e-6f) {
            dx /= n;
            dy /= n;
        } else {
            dy = 1.0f;
        }
    } else if (e.shape == 1) {
        // Ring: outward at a uniform angle; magnet's "inward" is the
        // caller passing a negated dir as speed sign.
        const float ang = rng_range(0.0f, 2.0f * kPi);
        dx = std::cos(ang);
        dy = std::sin(ang);
        pos_x_[slot] = x + dx * e.ring_r;
        pos_y_[slot] = y + dy * e.ring_r;
    } else {
        // Cone around the axis.
        const float base = std::atan2(e.cone_dir[1], e.cone_dir[0]);
        const float off = rng_range(-e.cone_spread_deg, e.cone_spread_deg) * kPi / 180.0f;
        dx = std::cos(base + off);
        dy = std::sin(base + off);
        pos_x_[slot] = x;
        pos_y_[slot] = y;
    }
    if (e.shape != 1) {
        pos_x_[slot] = x;
        pos_y_[slot] = y;
    }

    vel_x_[slot] = dx * speed;
    vel_y_[slot] = dy * speed;
    life_[slot] = rng_range(e.life_min, e.life_max);
    life0_[slot] = life_[slot];
    size_[slot] = e.size0;
    size0_[slot] = e.size0;
    size1_[slot] = e.size1;
    color_[slot] = e.color0;
    color0_[slot] = e.color0;
    color1_[slot] = e.color1;
    spark_[slot] = e.spark;
    grav_[slot] = e.grav;
    drag_[slot] = e.drag;
    rot_[slot] = 0.0f;
    elong_[slot] = 1.0f;

    // §13.4: spawn brightness 1.4 (so bursts bloom); flash-reduction
    // applies ×0.6 at spawn.
    const float brightness = flash_reduction_ ? 1.4f * 0.6f : 1.4f;
    color_[slot] = scale_rgba(color_[slot], brightness);
    color0_[slot] = scale_rgba(color0_[slot], brightness);

    ++spawned_total_;
}

void ParticleSystem::update(float dt) {
    // §13.1 integration, packed compaction (live slots stay dense).
    uint32_t w = 0;
    for (uint32_t i = 0; i < live_; ++i) {
        life_[i] -= dt;
        if (life_[i] <= 0.0f) {
            continue;
        }
        // Integrate.
        vel_x_[i] *= 1.0f / (1.0f + drag_[i] * dt);
        vel_y_[i] *= 1.0f / (1.0f + drag_[i] * dt);
        vel_y_[i] -= grav_[i] * dt;
        pos_x_[i] += vel_x_[i] * dt;
        pos_y_[i] += vel_y_[i] * dt;

        const float t = 1.0f - life_[i] / life0_[i];
        size_[i] = size0_[i] + (size1_[i] - size0_[i]) * t;
        uint32_t c = lerp_rgba(color0_[i], color1_[i], t);
        // Tail fade-out: min(1, life / 0.1).
        const float fade = std::min(1.0f, life_[i] / 0.1f);
        c = scale_rgba(c, fade);
        color_[i] = c;

        // Spark orientation (§13.4).
        if (spark_[i]) {
            rot_[i] = std::atan2(vel_y_[i], vel_x_[i]);
            const float sp = std::sqrt(vel_x_[i] * vel_x_[i] + vel_y_[i] * vel_y_[i]);
            elong_[i] = std::clamp(sp / 0.5f, 1.0f, 8.0f);
        }

        if (w != i) {
            // Move to the packed write slot.
            pos_x_[w] = pos_x_[i];
            pos_y_[w] = pos_y_[i];
            vel_x_[w] = vel_x_[i];
            vel_y_[w] = vel_y_[i];
            life_[w] = life_[i];
            life0_[w] = life0_[i];
            size_[w] = size_[i];
            size0_[w] = size0_[i];
            size1_[w] = size1_[i];
            color_[w] = color_[i];
            color0_[w] = color0_[i];
            color1_[w] = color1_[i];
            spark_[w] = spark_[i];
            grav_[w] = grav_[i];
            drag_[w] = drag_[i];
            rot_[w] = rot_[i];
            elong_[w] = elong_[i];
        }
        ++w;
    }
    live_ = w;
}

} // namespace tb::render

namespace tb::render {
uint64_t particle_seed_ns() {
    return tb_now_ns();
}
} // namespace tb::render
