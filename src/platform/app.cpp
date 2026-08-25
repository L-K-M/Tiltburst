#include "platform/app.h"

#include "core/config.h"
#include "core/log.h"
#include "core/time.h"
#include "core/version.h"
#include "platform/gpu_device.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/overlay.h"
#include "render/renderer.h"
#include "render/sdl_gpu_renderer.h"
#include "sim/sim_thread.h"
#include "sim/snapshot.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif

namespace tb::app {

namespace {

std::atomic<bool> g_quit{false};

constexpr uint64_t kHeadlessProbeTicks = 5000; // 5 s bounded probe
constexpr size_t kFrameRing = 240;
constexpr int kRenderSmokeW = 1080;
constexpr int kRenderSmokeH = 1920;
constexpr float kOverlayColor[4] = {0.00f, 0xE5 / 255.f, 1.0f, 1.0f};

void request_quit(int) {
    g_quit.store(true);
}

void install_signal_handlers() {
#if !defined(_WIN32)
    signal(SIGINT, &request_quit);
    signal(SIGTERM, &request_quit);
#endif
}

// fps over a 1 s window + frame-time percentiles over the ring.
struct FrameStats {
    float fps = 0.0f;
    float last_ms = 0.0f;
    float p50_ms = 0.0f;
    float p99_ms = 0.0f;

    void update(double dt_s, std::deque<float>& ring) {
        accum_s_ += dt_s;
        ++frames_;
        if (accum_s_ >= 1.0 && frames_ > 0) {
            fps_ = float(double(frames_) / accum_s_);
            accum_s_ = 0.0;
            frames_ = 0;
        }
        // The "frame" readout is this frame's duration, not the ring max.
        last_ms = float(dt_s * 1000.0);
        if (!ring.empty()) {
            std::deque<float> sorted = ring;
            std::sort(sorted.begin(), sorted.end());
            p50_ms = sorted[sorted.size() / 2];
            p99_ms = sorted[(sorted.size() * 99) / 100];
        }
        fps = fps_;
    }

private:
    double accum_s_ = 0.0;
    int frames_ = 0;
    float fps_ = 0.0f;
};

// Matches a long-form flag in "--name value" or "--name=value" form.
// Returns true when the flag matched (value filled); on matched-but-missing
// value, sets *missing_value.
bool match_flag(
    int argc, char** argv, int& i, const char* name, std::string* value, bool* missing_value) {
    const std::string arg = argv[i];
    const std::string prefix = std::string("--") + name + "=";

    *missing_value = false;
    if (arg == "--" + std::string(name)) {
        if (i + 1 >= argc) {
            *missing_value = true;
            return true;
        }
        *value = argv[++i];
        return true;
    }
    if (arg.rfind(prefix, 0) == 0) {
        *value = arg.substr(prefix.size());
        return true;
    }
    return false;
}

} // namespace

CliOptions parse_cli(int argc, char** argv) {
    CliOptions cli;

    auto fail = [&](const std::string& reason) {
        cli.error = reason;
        return cli;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        // --version short-circuits the rest of the command line (§2).
        if (arg == "--version") {
            cli.version = true;
            return cli;
        }

        if (arg == "--headless") {
            cli.headless = true;
            continue;
        }
        if (arg == "--dev") {
            cli.dev = true;
            continue;
        }
        if (arg == "--render-smoke") {
            cli.render_smoke = true;
            continue;
        }

        bool missing_value = false;
        std::string value;

        if (match_flag(argc, argv, i, "table", &value, &missing_value)) {
            if (missing_value) {
                return fail("--table requires a slug");
            }
            cli.table = value;
            continue;
        }
        if (match_flag(argc, argv, i, "screenshot-dir", &value, &missing_value)) {
            if (missing_value) {
                return fail("--screenshot-dir requires a path");
            }
            cli.screenshot_dir = value;
            continue;
        }
        if (match_flag(argc, argv, i, "frames", &value, &missing_value)) {
            if (missing_value) {
                return fail("--frames requires a value");
            }
            cli.frames = std::atoi(value.c_str());
            if (cli.frames <= 0) {
                return fail("--frames must be > 0");
            }
            continue;
        }
        if (match_flag(argc, argv, i, "windowed", &value, &missing_value)) {
            if (missing_value ||
                std::sscanf(value.c_str(), "%dx%d", &cli.window_w, &cli.window_h) != 2 ||
                cli.window_w <= 0 || cli.window_h <= 0) {
                return fail("--windowed expects WxH (e.g. 540x1080)");
            }
            continue;
        }

        return fail("unknown flag: " + arg);
    }

    if (cli.render_smoke && cli.headless) {
        return fail("--render-smoke cannot be combined with --headless");
    }
    if (cli.render_smoke && cli.frames <= 0) {
        return fail("--render-smoke requires --frames > 0");
    }
    if (cli.render_smoke && cli.screenshot_dir.empty()) {
        return fail("--render-smoke requires --screenshot-dir");
    }
    return cli;
}

int run(const CliOptions& cli) {
    // §1 step 2: logger with the in-memory ring only.
    log::init(cli.dev ? LogLevel::Debug : LogLevel::Info);
    install_signal_handlers();

    // §1 step 3: SDL metadata + init. --headless never touches video/GPU/
    // audio. The smoke run initializes a dummy video driver when the
    // machine has no display server (16-testing-ci.md §5).
    SDL_SetAppMetadata("Tiltburst", version_string(), "com.tiltburst.tiltburst");
    if (cli.render_smoke) {
        if (SDL_getenv("DISPLAY") == nullptr && SDL_getenv("WAYLAND_DISPLAY") == nullptr &&
            SDL_getenv("SDL_VIDEODRIVER") == nullptr) {
            SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
        }
    }
    const Uint32 sdl_flags =
        cli.render_smoke ? SDL_INIT_VIDEO | SDL_INIT_EVENTS
                         : (cli.headless ? SDL_INIT_EVENTS
                                         : SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
    if (!SDL_Init(sdl_flags)) {
        TB_LOG_ERROR("main", "SDL_Init failed: {}", SDL_GetError());
        log::flush_now();
        // A smoke run that cannot initialize video is a "cannot run here"
        // skip, never a job failure (05 §2.1 exit-code contract).
        return cli.render_smoke ? 2 : 1;
    }

    // §1 steps 4–5: pref path and the file log sink (keep newest five).
    if (paths::init_pref()) {
        time_t t = ::time(nullptr);
        tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
        const std::filesystem::path file =
            paths::logs_dir() / ("tiltburst-" + std::string(stamp) + ".log");
        log::set_file(file);

        std::error_code ec;
        std::vector<std::filesystem::path> logs;
        for (auto& e : std::filesystem::directory_iterator(paths::logs_dir(), ec)) {
            logs.push_back(e.path());
        }
        std::sort(logs.begin(), logs.end());
        while (logs.size() > 5) {
            std::filesystem::remove(logs.front(), ec);
            logs.erase(logs.begin());
        }
    } else {
        TB_LOG_WARN("main", "SDL_GetPrefPath failed: {} — logging to stderr only", SDL_GetError());
    }

    TB_LOG_INFO("main", "tiltburst starting (cpus={})", SDL_GetNumLogicalCPUCores());

    // §1 step 6: settings.json (fallback: defaults on parse error).
    Settings settings = Settings::defaults();
    if (!paths::pref().empty()) {
        settings = Settings::load(paths::pref() / "settings.json");
    }

    int code = 0;

    if (cli.render_smoke) {
        // CI smoke run: GPU device without a window, offscreen frames,
        // machine-readable summary on stdout (16-testing-ci.md §5).
        auto renderer = render::make_sdl_gpu_renderer();
        auto* gpu = static_cast<tb::render::SdlGpuRenderer*>(renderer.get());

        if (!gpu->init_offscreen(kRenderSmokeW, kRenderSmokeH, cli.dev)) {
            code = 2; // no usable GPU backend — logged skip
        } else {
            tb::SimSnapshot snap;
            render::RenderFrame frame;
            frame.snapshot = &snap;
            frame.show_overlay = false;

            const int frames = cli.frames;
            const int warmup = frames > 60 ? 60 : std::max(0, frames - 1);
            std::vector<float> ms;
            ms.resize(size_t(frames));

            for (int i = 0; i < frames; ++i) {
                snap.tick = uint64_t(i);
                frame.snapshot = &snap;
                const uint64_t f0 = tb_now_ns();
                gpu->render_offscreen_frame(frame);
                ms[size_t(i)] = float(tb_now_ns() - f0) / 1e6f;
            }

            std::filesystem::path out_dir = cli.screenshot_dir;
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            const bool wrote = gpu->write_png(out_dir / "tiltburst_smoke.png");

            std::vector<float> timed(ms.begin() + warmup, ms.end());
            std::sort(timed.begin(), timed.end());
            double sum = 0.0;
            for (float v : timed) {
                sum += v;
            }
            const double mean = timed.empty() ? 0.0 : sum / double(timed.size());
            const double p99 =
                timed.empty() ? 0.0
                              : double(timed[size_t(std::ceil(0.99 * double(timed.size())) - 1)]);
            const double maxv = timed.empty() ? 0.0 : double(timed.back());
            const std::string backend = gpu->backend_name();
            const bool software = backend.find("lavapipe") != std::string::npos ||
                                  backend.find("llvmpipe") != std::string::npos ||
                                  backend.find("SwiftShader") != std::string::npos ||
                                  backend.find("WARP") != std::string::npos ||
                                  backend.find("Basic Render") != std::string::npos;

            std::printf("render_smoke: frames=%d warmup=%d mean_ms=%.3f "
                        "p99_ms=%.3f max_ms=%.3f backend=%s software=%d\n",
                        frames,
                        warmup,
                        mean,
                        p99,
                        maxv,
                        backend.c_str(),
                        software ? 1 : 0);
            std::fflush(stdout);

            if (!wrote) {
                TB_LOG_WARN("main", "smoke PNG not written");
            }
            renderer->shutdown();
            code = 0;
        }
    } else if (cli.headless) {
        // Bounded display-less probe (journal note): boot with no video/
        // GPU/audio, run the sim loop, report the tick rate, exit 0.
        tb::SnapshotBuffer snapshots;
        tb::SimThread sim;
        sim.start([&snapshots](uint64_t tick) {
            tb::SimSnapshot s;
            s.tick = tick;
            snapshots.publish(s);
        });
        const uint64_t start = tb_now_ns();
        // Wall-clock deadline keeps a wedged sim from hanging CI forever.
        while (sim.ticks_run() < kHeadlessProbeTicks && tb_now_ns() - start < 60'000'000'000ull &&
               !g_quit.load(std::memory_order_relaxed)) {
            sleep_until_ns(tb_now_ns() + 1'000'000); // poll at 1 kHz
        }
        sim.request_stop();
        sim.join();

        const double secs = double(tb_now_ns() - start) * 1e-9;
        const double rate = double(sim.ticks_run()) / std::max(secs, 1e-9);
        TB_LOG_INFO("main",
                    "headless probe: {} ticks in {:.3f}s = {:.1f} Hz "
                    "(overruns={}, dropped={})",
                    sim.ticks_run(),
                    secs,
                    rate,
                    sim.overruns(),
                    sim.dropped_ticks());
        code = 0;
    } else {
        // Windowed dev mode: single portrait window, GPU clear + overlay.
        platform::WindowPtr window =
            platform::create_window("Tiltburst", cli.window_w, cli.window_h);
        if (!window) {
            TB_LOG_ERROR("main", "SDL_CreateWindow failed: {}", SDL_GetError());
            log::flush_now();
            SDL_Quit();
            return 1; // ✗ step 8
        }

        auto renderer = render::make_sdl_gpu_renderer();
        render::RendererConfig rcfg;
        rcfg.playfield_window = window.get();
        rcfg.debug_device = cli.dev;
        rcfg.prefer_mailbox = settings.present_mode != "vsync";
        if (!renderer->init(rcfg)) {
            log::flush_now();
            SDL_Quit();
            return 1; // ✗ step 9
        }

        tb::SnapshotBuffer snapshots;
        tb::SimThread sim;
        sim.start([&snapshots](uint64_t tick) {
            tb::SimSnapshot s;
            s.tick = tick;
            snapshots.publish(s);
        });

        FrameStats stats;
        std::deque<float> ring;
        uint64_t last_frame_ns = tb_now_ns();
        uint64_t next_cap_ns = last_frame_ns;
        bool show_overlay = true;

        while (!g_quit.load(std::memory_order_acquire)) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    g_quit.store(true);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        g_quit.store(true);
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F1) {
                        show_overlay = !show_overlay;
                    }
                    break;
                default:
                    break;
                }
            }

            const tb::SimSnapshot snap = snapshots.acquire_latest();

            const uint64_t frame_start_ns = tb_now_ns();
            const double dt = double(frame_start_ns - last_frame_ns) * 1e-9;
            last_frame_ns = frame_start_ns;

            stats.update(dt, ring);

            render::RenderFrame frame;
            frame.snapshot = &snap;
            frame.wall_dt = float(dt);
            frame.show_overlay = show_overlay;

            static thread_local std::vector<tb::render::QuadInstance> quads;
            quads.clear();
            if (show_overlay) {
                tb::render::OverlayStats os;
                os.fps = stats.fps;
                os.frame_ms_last = stats.last_ms;
                os.frame_ms_p50 = stats.p50_ms;
                os.frame_ms_p99 = stats.p99_ms;
                os.tick_rate_hz = 1000.0f; // pinned by canon 5.3
                os.tick_us_p50 = 1000000.0f / std::max(os.tick_rate_hz, 1.f);
                os.tick_us_p99 = os.tick_us_p50;

                static tb::render::Overlay overlay;
                overlay.update(os);

                int w = 0;
                int h = 0;
                SDL_GetWindowSizeInPixels(window.get(), &w, &h);
                (void)w;

                float y = 12.f;
                for (size_t i = 0; i < tb::render::Overlay::kLineCount; ++i) {
                    overlay.emit_quads(12.f,
                                       y,
                                       overlay.line(i),
                                       kOverlayColor[0],
                                       kOverlayColor[1],
                                       kOverlayColor[2],
                                       1.f,
                                       &quads,
                                       uint32_t(h));
                    y += 16.f;
                }
            }

            frame.quads = quads.data();
            frame.quad_count = uint32_t(quads.size());

            renderer->render_playfield(frame);

            const float ms = float(tb_now_ns() - frame_start_ns) / 1e6f;
            ring.push_back(ms);
            while (ring.size() > kFrameRing) {
                ring.pop_front();
            }

            log::drain_to_file();

            // Frame cap (§5.1): default matches the display refresh
            // (unknown refresh ⇒ 60); -1 disables the cap entirely.
            if (settings.max_fps != -1) {
                double refresh_hz = settings.max_fps > 0 ? double(settings.max_fps) : 60.0;
                if (settings.max_fps == 0) {
                    SDL_DisplayID display = SDL_GetDisplayForWindow(window.get());
                    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display)) {
                        if (mode->refresh_rate > 0) {
                            refresh_hz = mode->refresh_rate;
                        }
                    }
                }
                const uint64_t cap_ns = uint64_t(1e9 / refresh_hz);
                next_cap_ns = std::max(next_cap_ns + cap_ns, frame_start_ns);
                if (uint64_t now = tb_now_ns(); now < next_cap_ns) {
                    sleep_until_ns(next_cap_ns);
                }
            }
        }

        sim.request_stop();
        sim.join();
        renderer->shutdown();
        window.reset();

        if (!paths::pref().empty()) {
            settings.save(paths::pref() / "settings.json");
        }
    }

    log::shutdown();
    SDL_Quit();
    return code;
}

} // namespace tb::app
