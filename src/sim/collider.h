#pragma once

#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>

// Static collider primitives baked at table build (08-physics.md §3.1).
namespace tb::sim {

struct Collider {
    enum class Kind : uint8_t { Segment = 0, Point = 1, Arc = 2 };

    Kind kind = Kind::Point;
    // Geometry (segment: a,b; point: a=center, radius=post radius;
    // arc: a=center, radius=R, [a0,a1] CCW).
    Vec2 a{};
    Vec2 b{};
    float radius = 0.0f;
    float a0 = 0.0f;
    float a1 = 0.0f;
    uint16_t element_id = 0;
    uint16_t sub_index = 0;
    MaterialId material = MaterialId::Wood;
    uint8_t layer = 0;
};

} // namespace tb::sim
