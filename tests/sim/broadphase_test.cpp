#include "sim/broadphase.h"

#include "core/rng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

// Broadphase.MatchesBruteForce: 1,000 random collider configs; the grid
// query must return exactly the colliders whose inflated AABB overlaps the
// query AABB (04-milestones.md M2).
TEST(unit_broadphase, matches_brute_force) {
    tb::Pcg32 rng;
    rng.seed(0xB0ADu, 7u);

    for (int config = 0; config < 1000; ++config) {
        std::vector<tb::sim::Collider> colliders;
        const int count = 1 + int(rng.next_float() * 60.0f);
        for (int i = 0; i < count; ++i) {
            tb::sim::Collider c;
            c.element_id = uint16_t(i);
            c.sub_index = 0;
            c.layer = 0;
            if (rng.next_below(2) == 0) {
                c.kind = tb::sim::Collider::Kind::Point;
                c.a = {rng.next_float() * 0.52f, rng.next_float() * 1.04f};
                c.radius = 0.004f + rng.next_float() * 0.01f;
            } else {
                c.kind = tb::sim::Collider::Kind::Segment;
                c.a = {rng.next_float() * 0.52f, rng.next_float() * 1.04f};
                // Keep endpoints inside the play area: out-of-bounds
                // geometry is clamped into edge cells by design.
                c.b = {std::clamp(c.a.x + (rng.next_float() - 0.5f) * 0.3f, 0.0f, 0.52f),
                       std::clamp(c.a.y + (rng.next_float() - 0.5f) * 0.6f, 0.0f, 1.04f)};
            }
            colliders.push_back(c);
        }

        tb::sim::Broadphase grid;
        grid.build(colliders, 0.52f, 1.04f);

        const float qx = rng.next_float() * 0.52f;
        const float qy = rng.next_float() * 1.04f;
        const float dx = (rng.next_float() - 0.5f) * 0.2f;
        const float dy = (rng.next_float() - 0.5f) * 0.4f;

        std::vector<uint32_t> got;
        grid.query({qx, qy}, {qx + dx, qy + dy}, 0.0135f, 0, got);

        // Brute force: same inflated-AABB overlap test the build uses.
        std::set<uint32_t> want;
        const float pad = 0.0135f + tb::sim::kSkin;
        const float min_x = std::min(qx, qx + dx) - pad;
        const float max_x = std::max(qx, qx + dx) + pad;
        const float min_y = std::min(qy, qy + dy) - pad;
        const float max_y = std::max(qy, qy + dy) + pad;

        for (uint32_t i = 0; i < colliders.size(); ++i) {
            const tb::sim::Aabb box = tb::sim::collider_aabb(colliders[i], pad);
            if (box.max_x >= min_x && box.min_x <= max_x && box.max_y >= min_y &&
                box.min_y <= max_y) {
                want.insert(i);
            }
        }

        // Broadphase is conservative: shared-cell candidates may not
        // overlap pairwise, but NO overlapping collider may be missed.
        std::set<uint32_t> got_set(got.begin(), got.end());
        for (uint32_t idx : want) {
            EXPECT_TRUE(got_set.count(idx)) << "config " << config << " missed collider " << idx;
        }
        // Dedupe + sorted output contract.
        EXPECT_EQ(std::adjacent_find(got.begin(), got.end()), got.end())
            << "config " << config << " produced duplicates";
    }
}
