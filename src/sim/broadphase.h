#pragma once

#include "sim/ccd.h"
#include "sim/math.h"
#include "sim/types.h"

#include <cstdint>
#include <vector>

// Static collider set + uniform-grid broadphase (08-physics.md §3.1, §3.7).
// Everything is baked at table build; queries allocate nothing.
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

struct Aabb {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

// Conservative bounds for a collider, inflated by `pad`. Public so tests
// can mirror the build/query math exactly.
Aabb collider_aabb(const Collider& c, float pad);

class Broadphase {
public:
    void build(const std::vector<Collider>& colliders, float width, float height);

    // Collects candidates for the ball's swept AABB (p_now → p_then),
    // inflated by r + kSkin, on one layer. Output sorted by
    // (element_id, sub_index); duplicates removed.
    void query(Vec2 p_now, Vec2 p_then, float r, uint8_t layer, std::vector<uint32_t>& out) const;

private:
    struct Cell {
        std::vector<uint32_t> colliders; // sorted by (element_id, sub_index)
    };

    int cell_x(float x) const { return int((x - origin_x_) / kGridCell); }

    int cell_y(float y) const { return int((y - origin_y_) / kGridCell); }

    std::vector<Cell> cells_;
    int grid_w_ = 0;
    int grid_h_ = 0;
    float origin_x_ = -0.05f;
    float origin_y_ = -0.15f;
};

} // namespace tb::sim
