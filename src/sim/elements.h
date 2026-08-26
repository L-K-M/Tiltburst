#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>

// M6 reactive/trigger element state (08-physics.md §6.2–§6.8). Instances
// live in SimState::elements, sized at build; the hot path never allocates.
namespace tb::sim {

enum class ElementKind : uint8_t {
    Slingshot = 0,
    PopBumper,
    StandupTarget,
    Rollover,
    Gate,
    Spinner,
};

// Shared cooldown + event bookkeeping. Cooldowns are tick counts converted
// from ms at load (§6 preamble).
struct ElementCommon {
    ElementKind kind = ElementKind::Slingshot;
    uint16_t table_id = 0xFFFF; // element index in TableDef::elements
    uint32_t cooldown_left = 0; // ticks until re-armed
    uint32_t cooldown_ticks = 80;
    uint8_t layer = 0;
};

struct SlingshotElem {
    ElementCommon common{};
    Vec2 face_a{};
    Vec2 face_b{};
    Vec2 face_normal{}; // active normal: left of a→b (§6.2)
    float kick_speed = 3.5f;
    uint32_t kick_visual_ticks = 0; // arm animation window
};

struct PopBumperElem {
    ElementCommon common{};
    Vec2 pos{};
    float radius = 0.031f;
    float kick_speed = 4.5f;
    uint32_t flash_ticks = 0; // skirt flash window
};

struct StandupTargetElem {
    ElementCommon common{};
    Vec2 face_a{};
    Vec2 face_b{};
    Vec2 face_normal{}; // outward normal (facing_deg)
    float min_speed = 0.3f;
};

struct RolloverElem {
    ElementCommon common{};
    Vec2 a{}; // capsule segment along facing_deg, length 0.05
    Vec2 b{};
    bool armed = true; // re-arm on leaving the 0.016 m exit capsule
};

enum class GateState : uint8_t { OneWay = 0, Open, Closed };

struct GateElem {
    ElementCommon common{};
    Vec2 a{}; // segment ⊥ facing_deg, length width
    Vec2 b{};
    Vec2 face_normal{}; // f̂ (facing_deg direction)
    GateState state = GateState::OneWay;
    bool mechanical = true;   // default one_way ⇒ tb.gate_* no-ops (§6.7)
    bool switch_armed = true; // re-arm 0.03 m from the segment
};

struct SpinnerElem {
    ElementCommon common{};
    Vec2 a{}; // trigger segment ⊥ facing_deg, length 0.025
    Vec2 b{};
    Vec2 face_normal{};         // f̂
    float plate_angle = 0.0f;   // rad, published for rendering
    float plate_omega = 0.0f;   // rad/s
    float rev_angle_acc = 0.0f; // angle since last revolution event
    bool crossing_armed = true; // one spin-up per plate transit
};

} // namespace tb::sim
