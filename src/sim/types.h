#pragma once

#include "core/rng.h"
#include "sim/math.h"

#include <cstdint>

// Global constants and state types (08-physics.md §1).
namespace tb::sim {

constexpr float kTickDt = 0.001f;         // s, fixed tick (canon §5.3)
constexpr float kBallRadius = 0.0135f;    // m
constexpr float kBallMass = 0.08f;        // kg
constexpr float kBallInertia = 5.832e-6f; // (2/5)mr², kg·m²
constexpr float kMaxSpeed = 12.0f;        // m/s velocity clamp (canon §5.3)
constexpr float kMaxSpin = 220.0f;        // rad/s |omega_z| clamp
constexpr float kGravity = 9.81f;         // m/s²
constexpr float kDragK = 0.002f;          // 1/m quadratic air drag
constexpr float kRollMu = 0.025f;         // rolling resistance coefficient
constexpr float kSpinDamp = 0.7f;         // 1/s free spin decay
constexpr float kSkin = 1e-4f;            // m separation kept after TOI
constexpr float kToiEps = 1e-9f;          // s TOI tie/validity epsilon
constexpr float kRestSpeed = 0.03f;       // m/s restitution cutoff
constexpr float kMaxToiIter = 8.0f;       // contacts per ball per tick
constexpr float kGridCell = 0.032f;       // broadphase cell size

constexpr float kDefaultSlopeDeg = 6.5f;
constexpr int kMaxBalls = 6;

enum class BallMode : uint8_t { Free = 0, Ramp = 1, Captured = 2 };

// Ball state (08-physics.md §1.2). RAMP/CAPTURED fields exist from the
// start so the layout is stable; they are inert until M8.
struct Ball {
    // identity
    uint8_t index = 0;
    bool live = false;
    // FREE mode
    Vec2 pos{};
    Vec2 vel{};
    float omega_z = 0.0f;
    uint8_t layer = 0;
    Vec2 last_safe_pos{};
    // RAMP mode
    BallMode mode = BallMode::Free;
    uint16_t ramp_elem = 0xFFFF;
    float s = 0.0f;
    float s_dot = 0.0f;
    // CAPTURED mode
    uint16_t holder_elem = 0xFFFF;
    uint32_t hold_ticks = 0;
};

// Materials (08-physics.md §4.3): e, mu_s, mu_k, spin_transfer.
enum class MaterialId : uint8_t { Wood = 0, Steel, Rubber, Plastic, FlipperRubber };

struct Material {
    float restitution = 0.30f;
    float mu_s = 0.25f;
    float mu_k = 0.15f;
    float spin_transfer = 0.60f;
};

// Registry rows for MaterialId order; ball-ball constants live in §8 code.
Material material_row(MaterialId id);

// Per-tick latched input: one bit per logical action (05 §9.1 indices),
// latched exactly once at tick step 1 (§2.2 rule 5).
struct TickInput {
    uint32_t buttons = 0;
};

} // namespace tb::sim
