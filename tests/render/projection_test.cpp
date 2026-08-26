#include "render/view.h"

#include <gtest/gtest.h>

#include <cmath>

// Projection.PortraitRotationMapsCorners: under rot=90 on a 1920×1080
// landscape panel, the table's bottom-left corner lands at the swapchain's
// bottom-right NDC corner and the top-right at top-left (06 §6.4).
TEST(unit_projection, portrait_rotation_maps_corners) {
    const auto view = tb::render::compute_view(
        90u, 1920u, 1080u, tb::render::kReferenceTableW, tb::render::kReferenceTableH);
    EXPECT_EQ(view.scene_w, 960u); // 0.52 × 1846.15 ≈ 960
    EXPECT_EQ(view.scene_h, 1920u);

    // Table corner in logical NDC, then rotated.
    auto corner_ndc = [&](float tx, float ty) {
        const float nx = tx / tb::render::kReferenceTableW * 2.0f - 1.0f;
        const float ny = ty / tb::render::kReferenceTableH * 2.0f - 1.0f;
        float ox = 0.0f, oy = 0.0f;
        tb::render::rotate_ndc(90u, nx, ny, ox, oy);
        return std::pair<float, float>{ox, oy};
    };

    // Flipper end (0,0): logical (-1,-1) → rot90 (-y,x) = (1,-1).
    const auto [fx, fy] = corner_ndc(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(fx, 1.0f);
    EXPECT_FLOAT_EQ(fy, -1.0f);

    // Top-right (w,h): logical (1,1) → rot90 = (-1,1).
    const auto [tx, ty] = corner_ndc(tb::render::kReferenceTableW, tb::render::kReferenceTableH);
    EXPECT_FLOAT_EQ(tx, -1.0f);
    EXPECT_FLOAT_EQ(ty, 1.0f);
}

// Projection.AspectFitLetterboxes: scene target covers exactly the play
// area; bars fill the remainder of the swapchain.
TEST(unit_projection, aspect_fit_letterboxes) {
    // Reference cabinet: landscape-reported panel, rot 90.
    const auto v90 = tb::render::compute_view(
        90u, 1920u, 1080u, tb::render::kReferenceTableW, tb::render::kReferenceTableH);
    // ppm = min(1080/0.52, 1920/1.04) = 2076.92? No: logical portrait is
    // lw=1080 (x), lh=1920 (y); ppm = min(2076.9, 1846.15) = 1846.15.
    EXPECT_NEAR(v90.ppm, 1846.1538f, 0.01f);
    EXPECT_EQ(v90.bar_x, 0.0f);
    EXPECT_NEAR(v90.bar_y, 60.0f, 0.51f) << "(1920-1800)/2 = 60 px bars";

    // Native portrait panel: the table aspect (1:2) is WIDER than the
    // panel (9:16), so ppm binds on height and side bars appear.
    const auto v0 = tb::render::compute_view(
        0u, 1080u, 1920u, tb::render::kReferenceTableW, tb::render::kReferenceTableH);
    EXPECT_NEAR(v0.ppm, 1846.1538f, 0.01f);
    EXPECT_EQ(v0.scene_w, 960u);
    EXPECT_EQ(v0.scene_h, 1920u);
    EXPECT_NEAR(v0.bar_x, 60.0f, 0.51f);
    EXPECT_FLOAT_EQ(v0.bar_y, 0.0f);

    // Ultra-wide landscape swapchain: horizontal bars only.
    const auto vw = tb::render::compute_view(
        0u, 2560u, 1080u, tb::render::kReferenceTableW, tb::render::kReferenceTableH);
    EXPECT_GT(vw.bar_x, 0.0f);
    EXPECT_FLOAT_EQ(vw.bar_y, 0.0f);
}
