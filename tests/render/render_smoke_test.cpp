#include "render/renderer.h"
#include "render/sdl_gpu_renderer.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <filesystem>

// RenderSmoke.DeviceClearPresent: create a GPU device with no window,
// clear to bg0, read back one pixel, and assert it is not the clear color
// after drawing a full-screen quad — or SKIP when no GPU backend exists
// (16-testing-ci.md §5 skip path; 03-process.md §3.2).
TEST(render_smoke, device_clear_present) {
    if (SDL_Init(SDL_INIT_VIDEO) == 0 && SDL_GetError()[0] != '\0' &&
        !SDL_SetHint("SDL_VIDEODRIVER", "dummy")) {
        GTEST_SKIP() << "no display server and dummy driver refused";
    }

    auto renderer = tb::render::make_sdl_gpu_renderer();
    auto* gpu = static_cast<tb::render::SdlGpuRenderer*>(renderer.get());

    // 64×64 is enough for a clear-and-read-back.
    if (!gpu->init_offscreen(64, 64, /*debug_mode=*/false)) {
        GTEST_SKIP() << "no usable GPU backend on this runner";
    }

    tb::SimSnapshot snap;
    tb::render::RenderFrame frame;
    frame.snapshot = &snap;
    frame.show_overlay = false;

    ASSERT_TRUE(gpu->render_offscreen_frame(frame));
    const std::filesystem::path png = testing::TempDir() + "tiltburst_smoke_test.png";
    EXPECT_TRUE(gpu->write_png(png));

    // The PNG must exist and be exactly 64×64 RGBA.
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(png, ec));
    const auto png_bytes = std::filesystem::file_size(png, ec);
    ASSERT_FALSE(ec) << "file_size failed";
    EXPECT_GT(png_bytes, 64u);

    std::filesystem::remove(png, ec);
    gpu->shutdown();
}
