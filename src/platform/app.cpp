#include "platform/app.h"

#include "core/config.h"
#include "core/log.h"
#include "core/time.h"
#include "core/version.h"
#include "platform/gpu_device.h"
#include "platform/input.h"
#include "platform/latency.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/overlay.h"
#include "render/renderer.h"
#include "render/sdl_gpu_renderer.h"
#include "sim/sim_thread.h"
#include "sim/snapshot.h"
#include "sim/solver.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

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

// Loads a table pack for the windowed path: accepts "tables/<slug>" or a
// bare slug; throws TableLoadError upward (caught at the call site).
struct LoadedTable {
    tb::table::TableDef def;
};

// §9/§14 input pipeline: producer sources in §9.8 priority order (raw
// first, SDL last), the latched level state, the R2.1 cumulative
// histogram, and the F3 record ring.
struct InputPipeline {
    tb::input::Keymap keymap;
    std::vector<std::unique_ptr<tb::input::InputSource>> owned;
    std::vector<tb::input::InputSource*> sources;
    tb::input::InputState state;
    tb::input::LatencyHistogram histogram;
    tb::input::LatencyRing ring;
    bool raw_active = false;
    uint64_t prev_press_max_ns = 0;

    // Starts the platform primary (§9.8) plus SDL; returns the raw
    // source's availability (SDL always runs — menus need it).
    bool start(const Settings& settings) {
        keymap = tb::input::build_keymap_from_settings(settings.bindings);
        tb::input::set_active_keymap(&keymap);

        std::unique_ptr<tb::input::InputSource> raw = tb::input::make_winraw_input_source();
        if (raw == nullptr) {
            raw = tb::input::make_evdev_input_source();
        }
        if (raw != nullptr && raw->start()) {
            raw_active = true;
            owned.push_back(std::move(raw));
        } else if (raw != nullptr) {
            TB_LOG_WARN("main", "raw input unavailable; SDL handles gameplay");
        }
        owned.push_back(tb::input::make_sdl_input_source());
        for (auto& src : owned) {
            sources.push_back(src.get());
        }
        return raw_active;
    }

    void stop() {
        for (auto& src : owned) {
            src->stop();
        }
        owned.clear();
        sources.clear();
    }
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
        if (arg == "--latency-test") {
            cli.latency_test = true;
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
    if (cli.latency_test && (cli.headless || cli.render_smoke)) {
        return fail("--latency-test cannot be combined with --headless/--render-smoke");
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
        tb::sim::SimState sim_state;
        if (!cli.table.empty()) {
            std::filesystem::path dir = cli.table;
            if (dir.is_relative() && !std::filesystem::is_directory(dir)) {
                dir = std::filesystem::path("tables") / cli.table;
            }
            try {
                tb::table::build_sim(tb::table::load_table(dir), sim_state);
            } catch (const tb::table::TableLoadError& e) {
                TB_LOG_ERROR("main", "table load failed: {} ({})", e.what(), e.json_pointer);
                log::flush_now();
                SDL_Quit();
                return 1;
            }
        } else {
            tb::sim::make_synthetic_scene(sim_state, 424242);
        }
        tb::sim::Solver solver;
        tb::SimThread sim;
        sim.start([&snapshots, &solver, &sim_state](uint64_t tick) {
            const tb::sim::TickInput input;
            solver.step(sim_state, input);

            tb::SimSnapshot snap;
            snap.tick = tick;
            snap.sim_time_s = double(tick) * 0.001;
            uint32_t n = 0;
            for (int i = 0; i < tb::sim::kMaxBalls; ++i) {
                const auto& b = sim_state.balls[i];
                if (!b.live || b.mode != tb::sim::BallMode::Free) {
                    continue;
                }
                snap.balls[n].x = b.pos.x;
                snap.balls[n].y = b.pos.y;
                snap.balls[n].vx = b.vel.x;
                snap.balls[n].vy = b.vel.y;
                snap.balls[n].omega = b.omega_z;
                snap.balls[n].flags = 1;
                ++n;
            }
            snap.ball_count = n;
            snapshots.publish(snap);
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
    } else if (cli.latency_test) {
        // §14.4 photodiode measurement mode: no table, black playfield,
        // 256×256 white flash at the viewport corner nearest table (0,0)
        // on every flipper press edge, one CSV row per flash, and a
        // trailing §14.1 histogram comment block written on exit.
        platform::WindowPtr window =
            platform::create_window("Tiltburst", cli.window_w, cli.window_h);
        if (!window) {
            TB_LOG_ERROR("main", "SDL_CreateWindow failed: {}", SDL_GetError());
            log::flush_now();
            SDL_Quit();
            return 1;
        }
        auto renderer = render::make_sdl_gpu_renderer();
        render::RendererConfig rcfg;
        rcfg.playfield_window = window.get();
        rcfg.debug_device = cli.dev;
        rcfg.prefer_mailbox = settings.present_mode != "vsync";
        if (!renderer->init(rcfg)) {
            log::flush_now();
            SDL_Quit();
            return 1;
        }

        InputPipeline input;
        input.start(settings);
        tb::SnapshotBuffer snapshots;

        // Empty scene: no balls, no flippers — flashes only.
        tb::sim::SimState sim_state;
        sim_state.slope_deg = tb::sim::kDefaultSlopeDeg;
        sim_state.grid.build(sim_state.colliders, sim_state.width, sim_state.height);
        tb::sim::Solver solver;

        time_t t = ::time(nullptr);
        tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
        const std::filesystem::path csv_path =
            paths::logs_dir() / ("latency-" + std::string(stamp) + ".csv");
        FILE* csv = std::fopen(csv_path.string().c_str(), "w");
        if (csv != nullptr) {
            std::fprintf(csv, "input_ts_ns,latch_ts_ns,present_ns\n");
        }

        constexpr uint64_t kFlashMs = 100; // §14.4 flash duration
        std::atomic<uint64_t> flash_until_ns{0};
        std::atomic<uint64_t> flash_input_ts{0};
        std::atomic<uint64_t> flash_latch_ts{0};
        std::atomic<bool> flash_row_pending{false};

        tb::SimThread sim;
        sim.start([&](uint64_t tick) {
            const uint64_t latch_ts = tb_now_ns();
            const uint32_t buttons = tb::input::latch_input(input.sources.data(),
                                                            input.sources.size(),
                                                            input.state,
                                                            latch_ts,
                                                            &input.histogram);
            solver.step(sim_state, tb::sim::TickInput{buttons});
            snapshots.publish(tb::SimSnapshot{});

            // New left/right flipper press edge ⇒ trigger a flash.
            static thread_local uint32_t prev_flipper = 0;
            const uint32_t flipper = buttons & ((1u << tb::input::kActionLeftFlipper) |
                                                (1u << tb::input::kActionRightFlipper));
            const uint32_t rising = flipper & ~prev_flipper;
            prev_flipper = flipper;
            if (rising != 0) {
                flash_input_ts.store(input.state.last_press_ns[flipper == (1u << 1) ? 1 : 0],
                                     std::memory_order_release);
                flash_latch_ts.store(latch_ts, std::memory_order_release);
                flash_until_ns.store(latch_ts + kFlashMs * 1'000'000ull, std::memory_order_release);
                flash_row_pending.store(true, std::memory_order_release);
            }
        });

        FrameStats stats;
        std::deque<float> ring;
        bool show_overlay = false;
        while (!g_quit.load(std::memory_order_acquire)) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    g_quit.store(true);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    tb::input::sdl_key_input(input.sources.back(),
                                             input.keymap,
                                             static_cast<int>(event.key.scancode),
                                             event.type == SDL_EVENT_KEY_DOWN,
                                             event.key.repeat != 0,
                                             tb_now_ns());
                    if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        g_quit.store(true);
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F3) {
                        show_overlay = !show_overlay;
                    }
                    break;
                default:
                    break;
                }
            }

            const tb::SimSnapshot snap = snapshots.acquire_latest();
            const uint64_t frame_start_ns = tb_now_ns();
            stats.update(0.0, ring); // frame stats unused in this mode

            render::RenderFrame frame;
            frame.snapshot = &snap;
            frame.show_overlay = false;

            static thread_local std::vector<tb::render::QuadInstance> quads;
            quads.clear();

            const uint64_t now = tb_now_ns();
            if (now < flash_until_ns.load(std::memory_order_acquire)) {
                // 256×256 px pure-white square at the corner of the
                // viewport nearest table-space origin (§14.4).
                quads.push_back(
                    tb::render::QuadInstance{128.f, 128.f, 128.f, 128.f, 1.f, 1.f, 1.f, 1.f});
            }

            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window.get(), &w, &h);
            (void)w;

            // §14.4: the F3 overlay works in this mode.
            if (show_overlay) {
                static tb::render::Overlay overlay;
                tb::render::LatencyOverlayStats los;
                los.input_source = input.raw_active ? input.sources.front()->name() : "sdl";
                los.p999_ms = input.histogram.percentile(0.999) / 1e6;
                los.press_edges = input.histogram.total();
                overlay.update_latency(los);
                float y = 144.f; // below the flash square's corner
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

            const uint64_t present_ns = tb_now_ns();
            input.ring.complete_main(snap.tick, frame_start_ns, present_ns);

            // First frame that drew this flash writes the CSV row.
            if (flash_row_pending.exchange(false, std::memory_order_acq_rel) && csv != nullptr) {
                std::fprintf(csv,
                             "%llu,%llu,%llu\n",
                             static_cast<unsigned long long>(flash_input_ts.load()),
                             static_cast<unsigned long long>(flash_latch_ts.load()),
                             static_cast<unsigned long long>(present_ns));
                std::fflush(csv);
            }

            log::drain_to_file();
            sleep_until_ns(now + 1'000'000); // unthrottled-ish pacing is fine here
        }

        sim.request_stop();
        sim.join();
        input.stop();

        if (csv != nullptr) {
            // Trailing §14.1 histogram comment block.
            const auto snap_hist = input.histogram.snapshot();
            for (int i = 0; i < tb::input::LatencyHistogram::kBinCount; ++i) {
                if (snap_hist.bins[i] != 0) {
                    std::fprintf(
                        csv,
                        "# bin_us_upper=%llu,count=%llu\n",
                        static_cast<unsigned long long>(
                            (uint64_t(i) + 1) * tb::input::LatencyHistogram::kBinWidthNs / 1000ull),
                        static_cast<unsigned long long>(snap_hist.bins[i]));
                }
            }
            const uint64_t n = input.histogram.total();
            if (n >= 10'000) {
                std::fprintf(csv,
                             "# n=%llu, p50=%.3f, p99=%.3f, p99.9=%.3f ms\n",
                             static_cast<unsigned long long>(n),
                             input.histogram.percentile(0.50) / 1e6,
                             input.histogram.percentile(0.99) / 1e6,
                             input.histogram.percentile(0.999) / 1e6);
            } else {
                std::fprintf(csv, "# n=%llu INSUFFICIENT\n", static_cast<unsigned long long>(n));
            }
            std::fclose(csv);
            TB_LOG_INFO("main", "latency-test CSV written: {}", csv_path.string());
        }

        renderer->shutdown();
        window.reset();
    } else {
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
        tb::sim::SimState sim_state;
        std::unique_ptr<LoadedTable> loaded_table;
        std::filesystem::path table_dir;
        if (!cli.table.empty()) {
            table_dir = cli.table;
            if (table_dir.is_relative() && !std::filesystem::is_directory(table_dir)) {
                table_dir = std::filesystem::path("tables") / cli.table;
            }
            try {
                loaded_table = std::make_unique<LoadedTable>();
                loaded_table->def = tb::table::load_table(table_dir);
                tb::table::build_sim(loaded_table->def, sim_state);
                TB_LOG_INFO("main",
                            "table '{}' loaded: {} elements",
                            loaded_table->def.slug,
                            loaded_table->def.elements.size());
            } catch (const tb::table::TableLoadError& e) {
                TB_LOG_ERROR("main", "table load failed: {} ({})", e.what(), e.json_pointer);
                log::flush_now();
                SDL_Quit();
                return 1;
            }
        } else {
            tb::sim::make_synthetic_scene(sim_state, 424242);
        }
        tb::sim::Solver solver;

        InputPipeline input;
        input.start(settings);

        auto tick_fn = [&snapshots, &solver, &sim_state, &input](uint64_t tick) {
            // §2.1 step 1: late-latch the freshest input exactly once.
            const uint64_t latch_ts = tb_now_ns();
            const uint32_t buttons = tb::input::latch_input(input.sources.data(),
                                                            input.sources.size(),
                                                            input.state,
                                                            latch_ts,
                                                            &input.histogram);
            solver.step(sim_state, tb::sim::TickInput{buttons});

            tb::SimSnapshot snap;
            snap.tick = tick;
            snap.sim_time_s = double(tick) * 0.001;
            uint32_t n = 0;
            for (int i = 0; i < tb::sim::kMaxBalls; ++i) {
                const auto& b = sim_state.balls[i];
                if (!b.live || b.mode != tb::sim::BallMode::Free) {
                    continue;
                }
                snap.balls[n].x = b.pos.x;
                snap.balls[n].y = b.pos.y;
                snap.balls[n].vx = b.vel.x;
                snap.balls[n].vy = b.vel.y;
                snap.balls[n].omega = b.omega_z;
                snap.balls[n].flags = 1;
                ++n;
            }
            snap.ball_count = n;
            snapshots.publish(snap);

            // §14.1 stages 1–3 of this tick's latency record.
            uint64_t cur_press_max = 0;
            for (size_t a = 0; a < input.state.last_press_ns.size(); ++a) {
                cur_press_max = std::max(cur_press_max, input.state.last_press_ns[a]);
            }
            tb::input::LatencyRecord rec;
            rec.tick = tick;
            if (cur_press_max > input.prev_press_max_ns) {
                rec.input_ts_ns = cur_press_max; // newest press edge consumed
            }
            input.prev_press_max_ns = std::max(input.prev_press_max_ns, cur_press_max);
            rec.latch_ts_ns = latch_ts;
            rec.publish_ts_ns = tb_now_ns();
            input.ring.submit_sim(rec);
        };

        tb::SimThread sim;
        sim.start(tick_fn);

        FrameStats stats;
        std::deque<float> ring;
        int debug_level = 0;       // F2: 0 off, 1 colliders, 2 +broadphase
        bool show_latency = false; // F3 latency-detail page (§14.3)

        // Render-side mirror of the scene colliders (static after build;
        // the sim thread owns its own instance).
        tb::sim::SimState render_scene;
        if (loaded_table != nullptr) {
            tb::table::build_sim(loaded_table->def, render_scene);
        } else {
            tb::sim::make_synthetic_scene(render_scene, 424242);
        }
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
                case SDL_EVENT_KEY_UP: {
                    // §9.4 producer hook (before any UI handling); stamped
                    // at pump time over the tb::now_ns() base (ADR-019).
                    tb::input::sdl_key_input(input.sources.back(),
                                             input.keymap,
                                             static_cast<int>(event.key.scancode),
                                             event.type == SDL_EVENT_KEY_DOWN,
                                             event.key.repeat != 0,
                                             tb_now_ns());
                    if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        g_quit.store(true);
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F1) {
                        show_overlay = !show_overlay;
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F2) {
                        debug_level = (debug_level + 1) % 3; // 16.1 cycle
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F3) {
                        show_latency = !show_latency;
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F5 &&
                               loaded_table != nullptr) {
                        // M5 hot-reload: stop the sim, rebuild, restart.
                        sim.request_stop();
                        sim.join();
                        try {
                            loaded_table->def = tb::table::load_table(table_dir);
                            tb::table::build_sim(loaded_table->def, sim_state);
                            tb::table::build_sim(loaded_table->def, render_scene);
                            TB_LOG_INFO("main",
                                        "table reloaded: {} elements",
                                        loaded_table->def.elements.size());
                        } catch (const tb::table::TableLoadError& e) {
                            TB_LOG_ERROR("main",
                                         "reload failed (kept old table): {} ({})",
                                         e.what(),
                                         e.json_pointer);
                        }
                        sim.start(tick_fn);
                    } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F12) {
                        renderer->request_screenshot(
                            (paths::pref() / "screenshots" /
                             ("tiltburst_" + std::to_string(tb_now_ns()) + ".png"))
                                .string()
                                .c_str());
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    tb::input::g_app_focused.store(true, std::memory_order_relaxed);
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    tb::input::g_app_focused.store(false, std::memory_order_relaxed);
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
                int w = 0;
                int h = 0;
                SDL_GetWindowSizeInPixels(window.get(), &w, &h);
                (void)w;

                static tb::render::Overlay overlay;
                float y = 12.f;

                if (show_latency) {
                    // §14.3 F3 page: per-stage percentiles over the recent
                    // record window + the cumulative gate numbers.
                    tb::input::LatencyRecord recs[64];
                    const size_t n_recs = input.ring.copy_recent(recs, 64);
                    std::vector<double> ages;
                    for (size_t i = 0; i < n_recs; ++i) {
                        if (recs[i].input_ts_ns != 0 &&
                            recs[i].input_ts_ns <= recs[i].latch_ts_ns) {
                            ages.push_back(double(recs[i].latch_ts_ns - recs[i].input_ts_ns) / 1e6);
                        }
                    }
                    std::sort(ages.begin(), ages.end());
                    tb::render::LatencyOverlayStats los;
                    los.input_source = input.raw_active ? input.sources.front()->name() : "sdl";
                    los.input_latch_ms_p50 = ages.empty() ? 0.f : float(ages[ages.size() / 2]);
                    los.input_latch_ms_p95 =
                        ages.empty() ? 0.f : float(ages[(ages.size() * 95) / 100]);
                    los.input_latch_ms_max = ages.empty() ? 0.f : float(ages.back());
                    los.p999_ms = input.histogram.percentile(0.999) / 1e6;
                    los.press_edges = input.histogram.total();
                    overlay.update_latency(los);
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
                } else {
                    tb::render::OverlayStats os;
                    os.fps = stats.fps;
                    os.frame_ms_last = stats.last_ms;
                    os.frame_ms_p50 = stats.p50_ms;
                    os.frame_ms_p99 = stats.p99_ms;
                    os.tick_rate_hz = 1000.0f; // pinned by canon 5.3
                    os.tick_us_p50 = 1000000.0f / std::max(os.tick_rate_hz, 1.f);
                    os.tick_us_p99 = os.tick_us_p50;

                    overlay.update(os);

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
            }

            if (debug_level >= 1) {
                // Insert lights drawn as debug circles (04-milestones.md M5).
                for (const auto& light : render_scene.lights) {
                    const float s = light.size * 0.5f;
                    quads.push_back(tb::render::QuadInstance{light.pos.x * 1000.f,
                                                             light.pos.y * 1000.f,
                                                             s * 1000.f,
                                                             s * 1000.f,
                                                             0.0f,
                                                             0.9f,
                                                             0.6f,
                                                             light.on ? 1.f : 0.35f});
                }
            }

            frame.quads = quads.data();
            frame.quad_count = uint32_t(quads.size());
            frame.show_colliders = debug_level >= 1;
            frame.debug_colliders = render_scene.colliders.data();
            frame.debug_collider_count = uint32_t(render_scene.colliders.size());

            renderer->render_playfield(frame);

            // §14.1 stages 4–5 on the record matching the rendered snapshot.
            input.ring.complete_main(snap.tick, frame_start_ns, tb_now_ns());

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
        input.stop();
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
