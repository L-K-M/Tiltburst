#pragma once

#include "sim/events.h"

#include <cstdint>

namespace tb::render {

// CPU particle system (06-rendering.md §13). Fixed SoA pool, capacity
// 8192 (TB_MAX_PARTICLES); when full, new spawns steal the oldest live
// particle. Updated on the main thread at render rate; visual-only,
// never feeds the sim. RNG is a render-owned xorshift64* seeded from the
// wall clock — not tb.rng.
class ParticleSystem {
public:
    static constexpr uint32_t kMaxParticles = 8192;

    // §13.3 emitter row. Canonical effects (§13.4) are pre-built.
    struct Emitter {
        bool rate_mode = false;     // false: burst count
        int burst = 0;              // particles per spawn event
        double rate = 0.0;          // particles/s (rate mode)
        uint8_t shape = 0;          // 0 point, 1 ring, 2 cone
        float ring_r = 0.0f;        // ring radius
        float cone_dir[2] = {0, 1}; // cone axis (unit)
        float cone_spread_deg = 60.0f;
        float speed_min = 0.5f, speed_max = 1.5f;
        float life_min = 0.2f, life_max = 0.5f;
        float size0 = 0.004f, size1 = 0.001f; // meters
        uint32_t color0 = 0xFFFFFFFF;         // start
        uint32_t color1 = 0xFFFFFFFF;         // end
        bool spark = false;                   // disc|spark
        float grav = 0.0f;                    // m/s² down-table
        float drag = 0.0f;                    // 1/s
    };

    // §13.4 canonical effects; palette colors resolved by the caller.
    enum class Effect : uint8_t {
        BumperHitBurst = 0,
        SlingFlash,
        RampTrail,
        BallTrail,
        DrainBurst,
        JackpotStarburst,
        MagnetSparks,
        TiltWarningFlash,
        Count
    };

    ParticleSystem();

    // Replaces the palette-resolved colors for the canonical effects;
    // indices follow the Palette role order (bg0..glow_white).
    void set_palette(const uint32_t (&roles)[8]);

    // Spawns `effect` at table-space (x, y). Returns the burst count
    // spawned (the pool may steal slots when full).
    // dir (optional, unit, table space): the SlingFlash cone's
    // orientation (the sling face normal). Other effects ignore it —
    // their shapes are inherent (ring/burst are radially symmetric,
    // DrainBurst's cone is always up-table per §13.4).
    int spawn(Effect effect, float x, float y, const float* dir = nullptr);

    // §13.1 integration; dt clamped to 33 ms by the caller contract.
    void update(float dt);

    // Flash-reduction (13 §Accessibility): halves the jackpot count and
    // applies brightness ×0.6 at spawn; toggled at any time.
    void set_flash_reduction(bool on) { flash_reduction_ = on; }

    // ---- instance read-out (the renderer builds its draw from these) ----
    uint32_t live() const { return live_; }

    // SoA column access for live slot i (0..live-1). Slots are packed
    // (no holes) between updates.
    const float* pos_x() const { return pos_x_; }

    const float* pos_y() const { return pos_y_; }

    const float* size() const { return size_; }

    const uint32_t* color() const { return color_; }

    const bool* spark() const { return spark_; }

    const float* rot() const { return rot_; }

    const float* elong() const { return elong_; }

    uint32_t spawned_total() const { return spawned_total_; }

    uint32_t stolen_total() const { return stolen_total_; }

private:
    void emit(const Emitter& e, float x, float y);

    // SoA columns (packed to `live_`).
    float pos_x_[kMaxParticles];
    float pos_y_[kMaxParticles];
    float vel_x_[kMaxParticles];
    float vel_y_[kMaxParticles];
    float life_[kMaxParticles];
    float life0_[kMaxParticles];
    float size_[kMaxParticles];
    float size0_[kMaxParticles], size1_[kMaxParticles];
    uint32_t color_[kMaxParticles];
    uint32_t color0_[kMaxParticles], color1_[kMaxParticles];
    bool spark_[kMaxParticles];
    float grav_[kMaxParticles], drag_[kMaxParticles];
    float rot_[kMaxParticles];
    float elong_[kMaxParticles];

    uint32_t live_ = 0;
    uint32_t oldest_ = 0; // steal cursor
    uint64_t rng_state_ = 0;
    uint64_t spawned_total_ = 0;
    uint64_t stolen_total_ = 0;
    bool flash_reduction_ = false;

    uint32_t palette_[8] = {}; // current role colors

    uint32_t rng_u32();
    float rng_range(float lo, float hi);
};

} // namespace tb::render
