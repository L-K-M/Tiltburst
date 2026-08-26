#pragma once

#include "sim/collider.h"
#include "sim/math.h"

#include <cstdint>
#include <vector>

// Static collider set + uniform-grid broadphase (08-physics.md §3.1, §3.7).
// Everything is baked at table build; queries allocate nothing.
namespace tb::sim {

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
    // Global (element_id, sub_index) rank per collider index, baked at
    // build so queries can emit the §3.7-sorted candidate buffer without
    // touching the collider array.
    std::vector<uint32_t> rank_;
    int grid_w_ = 0;
    int grid_h_ = 0;
    float origin_x_ = -0.05f;
    float origin_y_ = -0.15f;
};

} // namespace tb::sim
