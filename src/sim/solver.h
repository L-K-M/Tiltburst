#pragma once

#include "core/hash.h"
#include "core/rng.h"
#include "sim/broadphase.h"
#include "sim/ccd.h"
#include "sim/elements.h"
#include "sim/events.h"
#include "sim/flipper.h"
#include "sim/flipper_ccd.h"
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
    // Materials default to the canonical §4.3 rows; build_sim applies
    // per-table overrides over them.
    SimState();

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
    float live_catch_window_ticks = kLiveCatchWindowTicks;
    float live_catch_factor = kLiveCatchFactor;

    // Table elements (M5): plunger, drain sensors, trough, lights.
    bool has_plunger = false;
    PlungerState plunger{};
    std::vector<OutholeRegion> outholes;
    int trough_balls = 0;
    std::vector<LightState> lights;
    uint32_t serve_delay_ticks = 0; // drain → serve countdown (basic M5 loop)

    // Reactive/trigger elements (M6, 08-physics.md §6.2–§6.8).
    std::vector<SlingshotElem> slingshots;
    std::vector<PopBumperElem> pop_bumpers;
    std::vector<StandupTargetElem> standups;
    std::vector<RolloverElem> rollovers;
    std::vector<GateElem> gates;
    std::vector<SpinnerElem> spinners;
    std::vector<KickerElem> kickers;
    std::vector<DropBankElem> drop_banks;
    std::vector<CaptiveBallElem> captives;
    std::vector<BallLockElem> ball_locks;

    // Ball accounting: active (FREE) + trough + locked == ball_count.
    int ball_count = 4;
    int locked_balls = 0;
    BallSaveState ball_save{};

    // Balls.
    Ball balls[kMaxBalls]{};

    // Material table (08 §4.3 rows; build_sim applies per-table overrides
    // over the canonical defaults). Indexed by MaterialId.
    Material mats[5]{};

    // Flippers (M4): dynamic colliders bypassing the grid.
    std::vector<Flipper> flippers;

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
    // Runs one 1 ms tick: flippers → forces → push-out → CCD → events.
    void step(SimState& state, const TickInput& input);
    void step(SimState& state, const TickInput* input);

private:
    void step_body(SimState& s, const TickInput* input);

private:
    struct Contact {
        enum Kind : uint8_t { None = 0, Static = 1, Pair = 2, Flipper = 3, Captive = 4 };

        float toi = 0.0f;
        uint8_t kind = None;
        uint8_t ball = 0;
        uint8_t ball2 = 0;
        uint8_t captive_idx = 0;
        Vec2 normal{};
        const Collider* collider = nullptr;
        struct Flipper* flipper = nullptr;
    };

    Contact find_earliest(SimState& s, float t_cur);

    // Pseudo-collider backing blocking gates / spinner slow-walls in the
    // static resolution path (see find_earliest).
    Collider gate_pseudo_{};

    // Static contacts resolved this tick, for reactive elements (§6.2–6.4):
    // {ball, element (TableDef index), approach speed}.
    static constexpr size_t kContactLogCap = 64;

    struct LoggedContact {
        uint8_t ball;
        uint16_t element;
        float approach;
    };

    LoggedContact contact_log_[kContactLogCap]{};
    size_t contact_log_n_ = 0;
    float resolve_surface(SimState& s,
                          Ball& ball,
                          Vec2 normal,
                          const Material& mat,
                          Vec2 surface_vel,
                          float live_catch_scale); // returns approach speed
    void resolve_flipper(SimState& s, Ball& ball, Flipper& f, const FlipperHit& hit);
    void resolve_pair(SimState& s, Ball& a, Ball& b, Vec2 normal);
    void resolve_captive(SimState& s, Ball& ball, CaptiveBallElem& cap, Vec2 normal);
    void step_regions(SimState& s, const TickInput* input);
    void step_elements(SimState& s);
    void step_lifecycle(SimState& s, const TickInput* input);
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
