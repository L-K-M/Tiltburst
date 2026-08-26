#include "sim/broadphase.h"

#include <algorithm>

namespace tb::sim {

Aabb collider_aabb(const Collider& c, float pad) {
    switch (c.kind) {
    case Collider::Kind::Segment:
        return {std::min(c.a.x, c.b.x) - pad,
                std::min(c.a.y, c.b.y) - pad,
                std::max(c.a.x, c.b.x) + pad,
                std::max(c.a.y, c.b.y) + pad};
    case Collider::Kind::Point:
        return {c.a.x - c.radius - pad,
                c.a.y - c.radius - pad,
                c.a.x + c.radius + pad,
                c.a.y + c.radius + pad};
    case Collider::Kind::Arc:
        // Conservative: full circle bounds.
        return {c.a.x - c.radius - pad,
                c.a.y - c.radius - pad,
                c.a.x + c.radius + pad,
                c.a.y + c.radius + pad};
    }
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

namespace {

bool order_less(const std::vector<Collider>& cs, uint32_t l, uint32_t r) {
    const auto& a = cs[l];
    const auto& b = cs[r];
    if (a.element_id != b.element_id) {
        return a.element_id < b.element_id;
    }
    if (a.sub_index != b.sub_index) {
        return a.sub_index < b.sub_index;
    }
    return l < r;
}

} // namespace

void Broadphase::build(const std::vector<Collider>& colliders, float width, float height) {
    grid_w_ = std::max(1, int((width - origin_x_) / kGridCell) + 1);
    grid_h_ = std::max(1, int((height - origin_y_) / kGridCell) + 1);
    cells_.assign(size_t(grid_w_) * size_t(grid_h_), Cell{});

    const float pad = kBallRadius + kSkin;
    for (uint32_t i = 0; i < colliders.size(); ++i) {
        if (colliders[i].layer != 0) {
            continue; // one grid per layer; M8 adds layer 1
        }
        const Aabb box = collider_aabb(colliders[i], pad);
        const int x0 = std::clamp(cell_x(box.min_x), 0, grid_w_ - 1);
        const int x1 = std::clamp(cell_x(box.max_x), 0, grid_w_ - 1);
        const int y0 = std::clamp(cell_y(box.min_y), 0, grid_h_ - 1);
        const int y1 = std::clamp(cell_y(box.max_y), 0, grid_h_ - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                cells_[size_t(y) * size_t(grid_w_) + size_t(x)].colliders.push_back(i);
            }
        }
    }
    for (Cell& cell : cells_) {
        std::sort(cell.colliders.begin(), cell.colliders.end(), [&](uint32_t l, uint32_t r) {
            return order_less(colliders, l, r);
        });
    }

    // Global (element_id, sub_index) ranking for query output ordering.
    rank_.assign(colliders.size(), 0);
    std::vector<uint32_t> order(colliders.size());
    for (uint32_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](uint32_t l, uint32_t r) {
        return order_less(colliders, l, r);
    });
    for (uint32_t pos = 0; pos < order.size(); ++pos) {
        rank_[order[pos]] = pos;
    }
}

void Broadphase::query(
    Vec2 p_now, Vec2 p_then, float r, uint8_t layer, std::vector<uint32_t>& out) const {
    out.clear();
    if (cells_.empty() || layer != 0) {
        return;
    }

    const float pad = r + kSkin;
    const float min_x = std::min(p_now.x, p_then.x) - pad;
    const float max_x = std::max(p_now.x, p_then.x) + pad;
    const float min_y = std::min(p_now.y, p_then.y) - pad;
    const float max_y = std::max(p_now.y, p_then.y) + pad;

    const int x0 = std::clamp(cell_x(min_x), 0, grid_w_ - 1);
    const int x1 = std::clamp(cell_x(max_x), 0, grid_w_ - 1);
    const int y0 = std::clamp(cell_y(min_y), 0, grid_h_ - 1);
    const int y1 = std::clamp(cell_y(max_y), 0, grid_h_ - 1);

    // Collect per-cell sorted lists, dedupe, then re-sort by the global
    // (element_id, sub_index) ranking — §3.7's output contract.
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (uint32_t idx : cells_[size_t(y) * size_t(grid_w_) + size_t(x)].colliders) {
                if (std::find(out.begin(), out.end(), idx) == out.end()) {
                    out.push_back(idx);
                }
            }
        }
    }

    const auto& rank = rank_;
    std::sort(
        out.begin(), out.end(), [&rank](uint32_t l, uint32_t r) { return rank[l] < rank[r]; });
}

} // namespace tb::sim
