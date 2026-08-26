#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>
#include <vector>

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
    Kicker,
    DropBank,
    CaptiveBall,
    BallLock,
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
    uint8_t last_ball = 0xFF;   // ball that crossed, for revolution events
};

enum class KickerStyle : uint8_t { Saucer = 0, Scoop, Vuk };

struct KickerElem {
    ElementCommon common{};
    Vec2 pos{};
    float radius = 0.014f;
    uint32_t capture_ticks = 800; // auto-eject failsafe (§6.9)
    float eject_speed = 3.0f;
    float eject_angle_deg = 90.0f;
    KickerStyle style = KickerStyle::Saucer;
    uint8_t held_ball = 0xFF; // captured ball index (0xFF = none)
    uint32_t hold_ticks = 0; // countdown to auto-eject; meaningful only while has_hold
    bool has_hold = false;
};

enum class DropTargetState : uint8_t { Up = 0, Dropping, Down, Raising };

struct DropBankElem {
    struct Target {
        Vec2 face_a{};
        Vec2 face_b{};
        Vec2 face_normal{};
        DropTargetState state = DropTargetState::Up;
        uint32_t anim_ticks = 0;
        uint32_t collider_idx = 0xFFFFFFFF; // colliders[] slot of the face
    };

    ElementCommon common{};
    std::vector<Target> targets;
    bool auto_reset = false;
    uint32_t auto_reset_ticks = 1500;
    uint32_t reset_timer = 0;      // counts after bank completion
    bool last_not_complete = true; // edge guard: emit BankComplete once
};

struct CaptiveBallElem {
    ElementCommon common{};
    Vec2 a{};
    Vec2 b{};
    Vec2 axis{}; // â = normalize(b − a)
    float slot_len = 0.0f;
    float s_c = 0.0f; // center position along â from a
    float s_dot = 0.0f;
    bool far_armed = true; // full-travel hysteresis (§6.13)
};

struct BallLockElem {
    ElementCommon common{};
    Vec2 pos{};
    int capacity = 2;
    float eject_speed = 2.5f;
    float eject_angle_deg = -90.0f;
    int held = 0;
    uint32_t release_timer = 0;  // one eject per 500 ms while releasing
    int release_pending = 0;     // balls still to eject this command
    uint32_t claim_ticks = 3000; // unclaimed auto-release failsafe
    bool claimed = false;        // set when a handler consumed the event
};

} // namespace tb::sim
