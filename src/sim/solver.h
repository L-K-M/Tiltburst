#pragma once

#include "core/hash.h"
#include "core/rng.h"
#include "sim/broadphase.h"
#include "sim/ccd.h"
#include "sim/events.h"
#include "sim/math.h"
#include "sim/types.h"

#include <vector>

// The simulation state and fixed-timestep solver (08-physics.md §1–§4, §8).
// tb_sim depends only on tb_core (canon §5.1); step() is allocation-free
// after build (03-process.md §1.6).
namespace tb::sim {

struct SimTickStats {
    float t_total_us = 0.0f;
    uint32_t toi_iters = 0;
    uint32_t frozen = 0;
};

struct SimState {
    // Table geometry (baked once at build; never mutated by step()).
    std::vector<Collider> colliders;
    Broadphase grid;
    float width = 0.52f;
    float height = 1.04f;

    // Physics parameters (§1.3 defaults; per-table overrides arrive M5).
    float slope_deg = kDefaultSlopeDeg;
    float mu_rr = kRollMu;
    float air_drag = kDragK;
    float restitution_falloff = 0.12f;
    float restitution_soft = 0.5f;

    // Balls.
    Ball balls[kMaxBalls]{};

    // RNG streams (05 §10.1): physics-side and script-side.
    Pcg32 rng_sim;
    Pcg32 rng_script;
    bool seeded = false;

    // Events + the rolling event-sequence hash (16-testing-ci.md §2.4.1).
    EventRing<SimEvent, 4096> render_ring;
    EventRing<SimEvent, 4096> game_ring;
    uint64_t event_seq_hash = kFnvOffset;

    uint64_t tick = 0;
    SimTickStats stats{};
};

class Solver {
public:
    // Runs one 1 ms tick: forces → push-out → CCD loop → events.
    void step(SimState& state, const TickInput& input);

private:
    struct Contact {
        enum Kind : uint8_t { None = 0, Static = 1, Pair = 2 };

        float toi = 0.0f;
        uint8_t kind = None;
        uint8_t ball = 0;
        uint8_t ball2 = 0;
        Vec2 normal{};
        const Collider* collider = nullptr;
    };

    Contact find_earliest(SimState& s, float t_cur);
    float resolve_static(SimState& s,
                         Ball& ball,
                         Vec2 normal,
                         const Material& mat); // returns approach speed
    void resolve_pair(SimState& s, Ball& a, Ball& b, Vec2 normal);
    void pushout(SimState& s);

    // Per-tick scratch, reused across ticks (never allocated in step()).
    std::vector<uint32_t> candidates_;
    uint32_t resolved_[kMaxBalls]{};
    bool frozen_[kMaxBalls]{};
};

// FNV-1a over the canonical serialization: live/mode/layer bytes, then raw
// IEEE-754 bit patterns of each ball's pos/vel/spin, both RNG stream
// states, and finally the event-sequence accumulator (16 §2.4.1).
uint64_t state_hash(const SimState& state);

// Builds the §2.9 synthetic perf scene: closed border, 8×10 rubber post
// lattice, four steel arcs, four balls with PCG32(seed=99) velocities.
void make_synthetic_scene(SimState& state, uint64_t game_seed);

} // namespace tb::sim
