#include "platform/app.h"

#include "audio/audio_engine.h"
#include "audio/audio_json.h"
#include "core/config.h"
#include "core/log.h"
#include "core/time.h"
#include "core/version.h"
#include "game/game_machine.h"
#include "game/high_scores.h"
#include "game/score_format.h"
#include "platform/backglass_pacer.h"
#include "platform/display_detect.h"
#include "platform/gpu_device.h"
#include "platform/input.h"
#include "platform/latency.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/art_renderer.h"
#include "render/backglass_renderer.h"
#include "render/overlay.h"
#include "render/renderer.h"
#include "render/sdl_gpu_renderer.h"
#include "render/tbart.h"
#include "sim/music_sink.h"
#include "sim/script_host.h"
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
#include <fstream>
#include <mutex>
#include <sstream>
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

// Resolves a --table argument: an explicit directory, or "tables/<slug>".
std::filesystem::path resolve_table_dir(const std::string& arg) {
    std::filesystem::path dir = arg;
    if (dir.is_relative() && !std::filesystem::is_directory(dir)) {
        dir = std::filesystem::path("tables") / arg;
    }
    return dir;
}

struct LoadedTable {
    tb::table::TableDef def;
    tb::sim::ScriptHost script;
    bool script_loaded = false;
};

// Loads table.json + rules.lua (when present) into `loaded`; returns
// false with the error logged. The ScriptHost is owned here and outlives
// the SimState reference it holds (both die with the windowed session).
bool load_table_pack(LoadedTable& loaded,
                     const std::filesystem::path& dir,
                     tb::sim::SimState& sim_state) {
    try {
        loaded.def = tb::table::load_table(dir);
        tb::table::build_sim(loaded.def, sim_state);
    } catch (const tb::table::TableLoadError& e) {
        TB_LOG_ERROR("main", "table load failed: {} ({})", e.what(), e.json_pointer);
        return false;
    }
    const std::filesystem::path rules = dir / "rules.lua";
    std::error_code ec;
    if (std::filesystem::exists(rules, ec)) {
        std::ifstream in(rules);
        if (!in) {
            TB_LOG_ERROR("main",
                         "rules.lua exists but is unreadable (continuing unscripted): {}",
                         rules.string());
            return true;
        }
        std::stringstream buf;
        buf << in.rdbuf();
        try {
            loaded.script.load(buf.str(), sim_state);
            sim_state.script = &loaded.script;
            loaded.script_loaded = true;
            TB_LOG_INFO("main", "rules.lua loaded for '{}'", loaded.def.slug);
        } catch (const std::exception& e) {
            TB_LOG_ERROR("main", "rules.lua failed (continuing unscripted): {}", e.what());
        }
    }
    return true;
}

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
        if (arg == "--audio-null") {
            cli.audio_null = true;
            continue;
        }
        if (arg == "--audio-latency-test") {
            cli.audio_latency_test = true;
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
            cli.windowed = true;
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
        // Declaration order is load-bearing: the ScriptHost owned by
        // loaded_table keeps a SimState& (script_host.h: "state must
        // outlive the host"; solver.h: "the owner must destroy it before
        // the state"), so sim_state is declared first and the host is
        // destroyed before it — same order as the test rig and the
        // windowed path below.
        tb::sim::SimState sim_state;
        LoadedTable loaded_table;
        if (!cli.table.empty()) {
            if (!load_table_pack(loaded_table, resolve_table_dir(cli.table), sim_state)) {
                log::flush_now();
                SDL_Quit();
                return 1;
            }
            // The M10 GameMachine owns the game lifecycle in the
            // windowed path: game_start fires from GameStarting on a
            // Start press, not at load. Scripts stay loaded-but-idle in
            // Attract (timers + ledger frozen, 11 §8.2).
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
            snap.tilt_px = sim_state.tilt.p.x;
            snap.tilt_py = sim_state.tilt.p.y;
            snap.tilt_vx = sim_state.tilt.v.x;
            snap.tilt_vy = sim_state.tilt.v.y;
            snap.tilt_abuse = sim_state.tilt.abuse_acc;
            snap.tilt_crossings = sim_state.tilt.crossings;
            snap.tilt_armed = uint8_t((sim_state.tilt.warn_armed ? 1u : 0u) |
                                      (sim_state.tilt.hard_armed ? 2u : 0u) |
                                      (sim_state.tilt.abuse_armed ? 4u : 0u));

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
        rcfg.bloom_strength = settings.composite_bloom_strength();
        rcfg.crt = settings.crt;
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

        // --- Display topology (07-displays.md, M12) ---
        platform::WindowPtr backglass_window;
        if (!cli.headless && !cli.windowed) {
            // Fullscreen cabinet path. --windowed dev mode (§11) skips
            // detection and creates its own second window below.
            // (bg_rotation rides on the Assignment and is consumed by
            // the M13 art pass — v1 backglass content is
            // orientation-agnostic by design.)
            //
            // displays.json loads ONLY on this path — headless and
            // windowed runs never read a config they cannot consume.
            platform::DisplaysConfig displays_cfg;
            bool displays_cfg_no_write = false; // corrupt OR unreadable:
                                                // never clobber it
            {
                const std::filesystem::path cfg_path =
                    paths::pref() / "displays.json"; // --display-config: M18
                std::error_code cfg_ec;
                const bool present = std::filesystem::exists(cfg_path, cfg_ec);
                if (cfg_ec) {
                    TB_LOG_WARN("main", "displays.json stat failed: {}", cfg_ec.message());
                    displays_cfg_no_write = true; // could not even stat:
                                                  // do not replace it
                } else if (present) {
                    std::ifstream cfg_in(cfg_path);
                    if (!cfg_in.good()) {
                        TB_LOG_WARN("main",
                                    "displays.json exists but cannot be read; using "
                                    "heuristics (auto-write skipped — fix permissions)");
                        displays_cfg_no_write = true;
                    } else {
                        const auto parsed = platform::load_displays_json(
                            std::string(std::istreambuf_iterator<char>(cfg_in),
                                        std::istreambuf_iterator<char>()));
                        if (parsed.corrupt) {
                            TB_LOG_WARN("main",
                                        "displays.json corrupt; using heuristics "
                                        "(auto-write skipped — fix the file)");
                            displays_cfg_no_write = true;
                        } else if (parsed.loaded) {
                            displays_cfg = parsed.cfg;
                        }
                    }
                }
            }

            std::vector<platform::DisplayInfo> displays;
            platform::Assignment assign;
            if (platform::enumerate_displays(displays)) {
                assign = platform::detect(displays, displays_cfg);
                for (const std::string& w : assign.warnings) {
                    TB_LOG_WARN("main", "displays: {}", w);
                }
                // 07 §5: the engine writes last_auto (and only
                // last_auto) after every successful auto-detection, so
                // stability survives restarts (cycle-11 review: the
                // read-only integration made the feature inert).
                const bool auto_playfield =
                    displays_cfg.playfield.match.empty() || displays_cfg.playfield.match == "auto";
                if (auto_playfield && !displays_cfg_no_write && !assign.stability_reused &&
                    assign.playfield >= 0) {
                    displays_cfg.last_auto.present = true;
                    displays_cfg.last_auto.playfield = displays[size_t(assign.playfield)].name;
                    displays_cfg.last_auto.backglass =
                        assign.backglass >= 0 ? displays[size_t(assign.backglass)].name : "";
                    // The full topology gates the next run's reuse
                    // (cycle-32): every attached display's name.
                    displays_cfg.last_auto.displays.clear();
                    for (const platform::DisplayInfo& d : displays) {
                        displays_cfg.last_auto.displays.push_back(d.name);
                    }
                    std::error_code write_ec;
                    std::filesystem::create_directories(paths::pref(), write_ec);
                    const std::string text = platform::save_displays_json(displays_cfg);
                    const std::filesystem::path tmp = paths::pref() / "displays.json.tmp";
                    const std::filesystem::path dst = paths::pref() / "displays.json";
                    std::ofstream out(tmp, std::ios::binary);
                    out.write(text.data(), std::streamsize(text.size()));
                    out.flush();
                    if (out.good()) {
                        out.close();
                        std::filesystem::rename(tmp, dst, write_ec);
                        if (write_ec) {
                            TB_LOG_WARN(
                                "main", "displays.json rename failed: {}", write_ec.message());
                            // out is closed above (before the rename);
                            // surface cleanup failures rather than
                            // discarding the reason.
                            std::error_code rm_ec;
                            std::filesystem::remove(tmp, rm_ec); // no orphan tmp
                            if (rm_ec) {
                                TB_LOG_WARN("main",
                                            "displays.json tmp cleanup failed for {}: {}",
                                            tmp.string(),
                                            rm_ec.message());
                            }
                        }
                    } else {
                        out.close();
                        TB_LOG_WARN("main", "displays.json write failed");
                        std::error_code rm_ec;
                        std::filesystem::remove(tmp, rm_ec);
                        if (rm_ec) {
                            TB_LOG_WARN(
                                "main", "displays.json tmp cleanup failed: {}", rm_ec.message());
                        }
                    }
                }
                // T13: single landscape display without a square
                // backglass reads as a desktop — say how to override.
                if (assign.backglass == -1 && assign.playfield >= 0 &&
                    displays[size_t(assign.playfield)].w >= displays[size_t(assign.playfield)].h) {
                    TB_LOG_WARN("main",
                                "landscape display without square backglass: assuming "
                                "desktop; set displays.json playfield.rotation for a "
                                "cabinet");
                }
                TB_LOG_INFO("main",
                            "displays: playfield={} backglass={} pf_rotation={}",
                            assign.playfield,
                            assign.backglass,
                            assign.pf_rotation);
            } else {
                TB_LOG_WARN("main", "no displays detected; playfield window only");
            }

            if (assign.backglass >= 0) {
                // 07 §7: borderless fullscreen via the CARRIED sdl_id —
                // never a second enumeration (the list can change
                // between calls; cycle-1 review).
                const platform::DisplayInfo& bg_display = displays[size_t(assign.backglass)];
                const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(bg_display.sdl_id);
                if (dm == nullptr) {
                    TB_LOG_WARN("main", "backglass display mode unavailable; no backglass");
                } else {
                    backglass_window = platform::create_fullscreen_window(
                        "Tiltburst Backglass", uint32_t(dm->w), uint32_t(dm->h), bg_display.sdl_id);
                    if (!backglass_window) {
                        TB_LOG_WARN("main", "backglass window creation failed: {}", SDL_GetError());
                    }
                }
            }
        } else if (cli.windowed && !cli.headless) {
            // §11 dev mode: a second 640x512 window (side positioning
            // lands with M18 menu work; v1 centers it). No displays.json
            // on this path — the dev window always exists.
            backglass_window = platform::create_window("Tiltburst Backglass", 640, 512);
        }

        auto renderer = render::make_sdl_gpu_renderer();
        render::RendererConfig rcfg;
        rcfg.playfield_window = window.get();
        rcfg.backglass_window = backglass_window.get();
        rcfg.debug_device = cli.dev;
        rcfg.bloom_strength = settings.composite_bloom_strength();
        rcfg.crt = settings.crt;
        rcfg.prefer_mailbox = settings.present_mode != "vsync";
        if (!renderer->init(rcfg)) {
            log::flush_now();
            SDL_Quit();
            return 1; // ✗ step 9
        }

        // Backglass content pipeline: layout + ~30 Hz pacer (07 §8).
        render::BackglassLayout bg_layout;
        // Invariant: Overlay's glyph emission is stateless
        // (stb_easy_font prints from baked metrics — no init()); if
        // that ever changes, backglass text silently renders nothing
        // and this declaration must init it.
        render::Overlay bg_font;
        platform::BackglassPacer bg_pacer;
        std::vector<render::QuadInstance> bg_built;

        tb::SnapshotBuffer snapshots;
        // Declaration order is load-bearing: the ScriptHost owned by
        // *loaded_table keeps a SimState& (script_host.h: "state must
        // outlive the host"; solver.h: "the owner must destroy it before
        // the state"), so sim_state is declared first and the host is
        // destroyed before it.
        tb::sim::SimState sim_state;
        std::unique_ptr<LoadedTable> loaded_table;
        std::filesystem::path table_dir;
        if (!cli.table.empty()) {
            table_dir = resolve_table_dir(cli.table);
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

        // Music bridge (12-audio.md §9, M14): the script host and the
        // GameMachine call this ON THE SIM THREAD; requests queue under
        // a mutex (song changes are human-rate) and the main loop
        // drains them into AudioSystem. Built before both the machine
        // and the audio system; `audio` binds after its init.
        struct MusicBridge final : sim::MusicSink {
            struct Request {
                std::string id; // empty = stop
            };

            std::mutex mu;
            std::vector<Request> pending;
            tb::audio::AudioSystem* audio = nullptr;

            void play_music(const char* song_id) override {
                if (song_id == nullptr || song_id[0] == '\0') {
                    return;
                }
                std::lock_guard<std::mutex> lock(mu);
                pending.push_back({song_id});
            }

            void stop_music() override {
                std::lock_guard<std::mutex> lock(mu);
                pending.push_back({});
            }

            // Main thread, once per frame.
            void drain() {
                std::vector<Request> take;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    take.swap(pending);
                }
                if (audio == nullptr) {
                    return;
                }
                for (const Request& r : take) {
                    // play_music owns the §9 semantics: unknown ids are
                    // SILENCE (it stops the current song, warn-once) —
                    // do not pre-filter here.
                    if (r.id.empty()) {
                        audio->stop_music();
                    } else {
                        (void)audio->play_music(r.id);
                    }
                }
            }
        };

        MusicBridge music_bridge;

        // M10 game framework (11-game-framework.md): only for a loaded
        // table with rules; synthetic scenes run bare.
        std::unique_ptr<tb::game::GameMachine> machine;
        tb::game::HighScoreTable high_scores;
        std::filesystem::path score_path;
        std::string score_slug;
        uint32_t score_retry_ticks = 0; // failed-save backoff (1 Hz)
        if (loaded_table && loaded_table->script_loaded) {
            tb::game::FrameworkConfig fcfg;
            fcfg.balls_per_game = settings.balls_per_game;
            fcfg.tilt_warnings = settings.tilt_warnings;
            fcfg.ball_save_ticks = uint32_t(std::clamp(settings.ball_save_seconds, 0, 15)) * 1000;
            fcfg.replay_score = loaded_table->def.replay_score;
            {
                const time_t now = ::time(nullptr);
                tm lt{};
#if defined(_WIN32)
                localtime_s(&lt, &now);
#else
                localtime_r(&now, &lt);
#endif
                char stamp[16];
                strftime(stamp, sizeof(stamp), "%Y-%m-%d", &lt);
                fcfg.date_stamp = stamp;
            }
            std::error_code mk_ec;
            std::filesystem::create_directories(paths::pref() / "scores", mk_ec);
            if (mk_ec) {
                TB_LOG_WARN("main", "cannot create scores directory: {}", mk_ec.message());
            }
            // The slug is third-party metadata and names a file we
            // read AND write: strip anything outside [A-Za-z0-9._-] so
            // no pack can escape the scores/ directory.
            std::string safe_slug;
            for (char c : loaded_table->def.slug) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-') {
                    safe_slug.push_back(c);
                }
            }
            if (safe_slug.empty() || safe_slug == "." || safe_slug == "..") {
                safe_slug = "table";
            }
            // Windows DOS device names resolve even with an extension
            // (con.json etc.); a slug that reduces to one is not usable
            // as a file name there.
            {
                std::string low = safe_slug;
                for (char& c : low) {
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                }
                std::string stem = low;
                const size_t dot = stem.find('.');
                if (dot != std::string::npos) {
                    stem = stem.substr(0, dot);
                }
                const bool reserved =
                    stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
                    (stem.size() == 4 &&
                     (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
                     stem[3] >= '1' && stem[3] <= '9');
                if (reserved) {
                    // Prefix with a char the sanitizer NEVER emits ('~'
                    // is stripped from real slugs above): a constant
                    // fallback would merge reserved-slug tables, and an
                    // allowed char like '-' or '_' stays forgeable by a
                    // table literally named "T-Com1".
                    safe_slug = "t~" + safe_slug;
                }
            }
            score_slug = safe_slug;
            score_path = paths::pref() / "scores" / (safe_slug + ".json");
            if (!high_scores.load(score_path)) {
                // §7: seed from meta.default_scores when the pack
                // declares it; otherwise the list simply starts empty.
                // A file that EXISTS but failed to load is corrupt —
                // keep a .bad copy (settings.json pattern) so a
                // transient read error can never silently eat a top 10.
                if (std::filesystem::exists(score_path)) {
                    const std::filesystem::path bad = score_path.string() + ".bad";
                    std::error_code bad_ec;
                    std::filesystem::rename(score_path, bad, bad_ec);
                    if (bad_ec) {
                        // A stale or locked .bad is the usual reason
                        // the rename failed; clear it and retry before
                        // giving up on the keep-copy.
                        std::error_code rm_ec;
                        std::filesystem::remove(bad, rm_ec);
                        std::filesystem::rename(score_path, bad, bad_ec);
                    }
                    if (bad_ec) {
                        // Both renames failed: leave the corrupt file
                        // in place — the crash-safe save's rename below
                        // replaces it atomically; destroying the only
                        // copy buys nothing.
                        TB_LOG_WARN("main",
                                    "score file corrupt; keep-copy failed: {} (the "
                                    "seeded save will replace it)",
                                    bad_ec.message());
                    } else {
                        TB_LOG_WARN("main", "score file corrupt; moved to {}", bad.string());
                    }
                }
                high_scores.seed_defaults(loaded_table->def.default_scores, fcfg.date_stamp);
                if (!high_scores.save(score_path, score_slug)) {
                    TB_LOG_WARN("main", "initial score seed write failed");
                }
            }
            machine = std::make_unique<tb::game::GameMachine>(
                loaded_table->script, sim_state, high_scores, fcfg);
            // §9 music seam: script (tb.play_music) + framework
            // (attract/game_over autoplay) route through the bridge.
            loaded_table->script.set_music_sink(&music_bridge);
            machine->set_music_sink(&music_bridge);
            sim_state.nudge_level = std::clamp(settings.nudge_level, 1, 3);
            sim_state.fsm_ctx = machine.get();
            sim_state.fsm_step = [](void* ctx, tb::sim::SimState& s, const tb::sim::TickInput& in) {
                static_cast<tb::game::GameMachine*>(ctx)->step(in);
            };
            TB_LOG_INFO("main", "game framework attached: table '{}'", loaded_table->def.slug);
        }

        // Audio (12-audio.md, M11): device ladder (or null backend),
        // bank + purpose map + intern table published at table load.
        tb::audio::AudioSystem audio;
        {
            tb::audio::AudioConfig acfg;
            acfg.master = settings.audio_master;
            acfg.sfx = settings.audio_sfx;
            acfg.music = settings.audio_music;
            acfg.ui = settings.audio_ui;
            acfg.period_frames = settings.audio_period_frames;
            acfg.null_backend = cli.audio_null;
            acfg.latency_probe = cli.audio_latency_test;
            if (!audio.init(acfg) && !cli.audio_null) {
                TB_LOG_WARN("main", "continuing without audio");
            }
            music_bridge.audio = &audio; // §9 bridge binds post-init
        }
        std::vector<std::string> intern_names;          // owns the strings
        std::vector<tb::sim::PatchIntern> patch_intern; // views into them
        if (loaded_table) {
            tb::audio::TableAudio table_audio;
            bool have_audio_json = false;
            try {
                have_audio_json = tb::audio::load_audio_json(table_dir, table_audio);
            } catch (const tb::audio::AudioLoadError& e) {
                TB_LOG_ERROR("main", "audio load failed: {} ({})", e.what(), e.json_pointer);
                log::flush_now();
                return 1;
            }
            int purpose_patch[tb::sim::SimState::kSoundPurposeCount] = {};
            for (int& p : purpose_patch) {
                p = -1; // no audio until the bank resolves defaults
            }
            std::unique_ptr<tb::audio::PatchBank> bank;
            try {
                bank = tb::audio::build_bank(table_audio, table_dir, purpose_patch);
            } catch (const tb::audio::AudioLoadError& e) {
                TB_LOG_ERROR("main", "bank build failed: {} ({})", e.what(), e.json_pointer);
                log::flush_now();
                return 1;
            }
            // The intern table OWNS its strings: the bank moves into
            // the audio system (retired banks are freed by pump()), so
            // borrowing name pointers from it would dangle.
            intern_names.clear();
            patch_intern.clear();
            intern_names.reserve(bank->size());
            patch_intern.reserve(bank->size());
            for (size_t i = 0; i < bank->size(); ++i) {
                intern_names.push_back(bank->patch_entries()[i].name);
            }
            for (size_t i = 0; i < bank->size(); ++i) {
                patch_intern.push_back({intern_names[i].c_str(), uint16_t(i)});
            }
            audio.publish_bank(std::move(bank));
            for (int i = 0; i < tb::sim::SimState::kSoundPurposeCount; ++i) {
                sim_state.sound_purpose_patch[i] = purpose_patch[i];
            }
        }
        sim_state.sound_queue = &audio.sound_queue();
        sim_state.patch_intern = patch_intern.data();
        sim_state.patch_intern_n = uint32_t(patch_intern.size());

        InputPipeline input;
        input.start(settings);

        auto tick_fn = [&snapshots,
                        &solver,
                        &sim_state,
                        &input,
                        &machine,
                        &high_scores,
                        &score_path,
                        &score_slug,
                        &score_retry_ticks,
                        &loaded_table,
                        &audio](uint64_t tick) {
            // §2.1 step 1: late-latch the freshest input exactly once.
            const uint64_t latch_ts = tb_now_ns();
            const uint32_t buttons = tb::input::latch_input(input.sources.data(),
                                                            input.sources.size(),
                                                            input.state,
                                                            latch_ts,
                                                            &input.histogram);
            const tb::sim::TickInput tick_input{buttons};
            const bool paused = machine && machine->state() == tb::game::GameState::Paused;
            if (paused) {
                // §8.5: physics + script ticks freeze; the FSM itself
                // keeps consuming commands.
                machine->step(tick_input);
            } else {
                solver.step(sim_state, tick_input); // fsm runs in phase 3
            }
            audio.publish_tick(tick); // 12 §4.2: newest completed tick
            if (machine && machine->scores_dirty()) {
                // Clear only after a successful write; a failing save
                // retries at 1 Hz rather than every tick (a full JSON
                // serialize per 1 kHz tick against a dead directory is
                // its own hazard).
                if (score_retry_ticks > 0) {
                    --score_retry_ticks;
                } else if (high_scores.save(score_path, score_slug)) {
                    machine->clear_scores_dirty();
                } else {
                    score_retry_ticks = 1'000;
                    TB_LOG_WARN_RATELIMITED("main", "score save failed; retrying at 1 Hz");
                }
            }

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
            snap.tilt_px = sim_state.tilt.p.x;
            snap.tilt_py = sim_state.tilt.p.y;
            snap.tilt_vx = sim_state.tilt.v.x;
            snap.tilt_vy = sim_state.tilt.v.y;
            snap.tilt_abuse = sim_state.tilt.abuse_acc;
            snap.tilt_crossings = sim_state.tilt.crossings;
            snap.tilt_armed = uint8_t((sim_state.tilt.warn_armed ? 1u : 0u) |
                                      (sim_state.tilt.hard_armed ? 2u : 0u) |
                                      (sim_state.tilt.abuse_armed ? 4u : 0u));

            // Game-layer fields for the backglass (07 §8): filled HERE,
            // on the sim thread that owns them — the render loop reads
            // only the published snapshot, never the live objects
            // (cycle-5 review: the old direct reads were a data race).
            if (machine != nullptr) {
                // machine exists only when loaded_table && script_loaded
                // (its construction site ~line 1060); the deref below is
                // invariant-safe.
                snap.game.player_count =
                    std::clamp(machine->player_count(), 1, decltype(snap.game)::kMaxPlayers);
                snap.game.current_player =
                    std::clamp(machine->current_player(), 1, snap.game.player_count);
                snap.game.ball_number = machine->player_count() > 0
                                            ? machine->player(snap.game.current_player).ball_number
                                            : 1; // attract: no live player yet
                snap.game.game_state = uint8_t(machine->state());
                for (int pi = 1; pi <= snap.game.player_count; ++pi) {
                    snap.game.scores[size_t(pi - 1)] = loaded_table->script.player_scores(pi).score;
                }
                // Attract page machine (§8.2): published for the
                // backglass layout.
                snap.game.attract_page = uint8_t(machine->attract_page());
                snap.game.attract_page_time_s = machine->attract_page_time_s();
            } else if (loaded_table) {
                // No framework attached: attract EXPLICITLY — never by
                // relying on GameState::Attract == 0.
                snap.game.game_state = uint8_t(game::GameState::Attract);
                snap.game.scores[0] = loaded_table->script.player_scores(1).score;
            }
            {
                // The high-score table mutates on THIS thread (insert
                // at game end via the FSM); copy the top 10 here so
                // the render loop never touches the live object
                // (cycle-27 review).
                snap.game.high_score_count = uint32_t(std::min<size_t>(
                    high_scores.entries().size(), decltype(snap.game)::kHighScoreCap));
                for (uint32_t i = 0; i < snap.game.high_score_count; ++i) {
                    snap.game.high_scores[i].initials[0] = high_scores.entries()[i].initials[0];
                    snap.game.high_scores[i].initials[1] = high_scores.entries()[i].initials[1];
                    snap.game.high_scores[i].initials[2] = high_scores.entries()[i].initials[2];
                    snap.game.high_scores[i].initials[3] = '\0';
                    snap.game.high_scores[i].score = high_scores.entries()[i].score;
                }
            }
            if (loaded_table) {
                const sim::BackglassModel& bm = loaded_table->script.backglass();
                snap.game.layout = bm.layout;
                snap.game.focus_player = bm.focus_player;
                snap.game.message_style = bm.message_style;
                // Clamp ONCE and store the clamped length — a longer
                // live message must not leave a stale tail in the copy.
                const uint32_t msg_len =
                    uint32_t(std::min(size_t(bm.message_len), sizeof(snap.game.message) - 1));
                snap.game.message_len = msg_len;
                std::memcpy(snap.game.message, bm.message, msg_len);
                snap.game.message[msg_len] = '\0';
            }
            // Light bitmap (M14): the sim-thread lights at phase-3
            // exit — script show or framework attract choreography —
            // published for the render thread's scene copy.
            {
                const uint32_t n =
                    uint32_t(std::min<size_t>(sim_state.lights.size(), SimSnapshot::kLightCap));
                snap.light_count = n;
                for (uint32_t i = 0; i < n; ++i) {
                    if (sim_state.lights[i].on) {
                        snap.light_bits[i >> 3] |= uint8_t(1u << (i & 7));
                    } else {
                        snap.light_bits[i >> 3] &= uint8_t(~(1u << (i & 7)));
                    }
                }
            }
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

        // M13a: load art.json (13 s3) and build the light-id map the
        // loader validates "light" fields against. The art renderer
        // reads live LightState rows from the SIM state each frame.
        render::TbArt table_art;
        bool table_art_loaded = false;
        std::vector<std::pair<std::string, int>> art_light_ids;
        if (loaded_table != nullptr && !cli.headless) {
            for (size_t i = 0; i < loaded_table->def.elements.size(); ++i) {
                const auto& el = loaded_table->def.elements[size_t(i)];
                if (std::holds_alternative<tb::table::LightDef>(el.def)) {
                    art_light_ids.emplace_back(el.id(), int(i));
                }
            }
            try {
                auto result = render::load_art(table_dir, art_light_ids);
                if (result.loaded) {
                    table_art = std::move(result.art);
                    table_art_loaded = true;
                    TB_LOG_INFO("main", "art.json loaded: {} layers", table_art.layers.size());
                }
            } catch (const render::ArtError& e) {
                // User-editable content: warn + greybox (the M11
                // audio.json policy), never fatal at startup.
                TB_LOG_WARN(
                    "main", "art.json corrupt ({} at {}); greybox", e.what(), e.json_pointer);
            }
        }
        render::ArtRenderer art_renderer;
        if (table_art_loaded) {
            art_renderer.set_art(&table_art);
        }
        uint64_t last_frame_ns = tb_now_ns();
        uint64_t next_cap_ns = last_frame_ns;
        bool show_overlay = true;

        while (!g_quit.load(std::memory_order_acquire)) {
            audio.pump();         // reclaim retired patch banks (12 §2.3)
            music_bridge.drain(); // §9: sim-thread music -> commands
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

            // M14: apply the snapshot's light bitmap to the render
            // scene copy — light-bound art follows the script show and
            // the §8.2 attract choreography.
            {
                const uint32_t n =
                    std::min<uint32_t>(uint32_t(render_scene.lights.size()), snap.light_count);
                for (uint32_t i = 0; i < n; ++i) {
                    render_scene.lights[i].on = (snap.light_bits[i >> 3] & (1u << (i & 7))) != 0;
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
                                                             0.5f});
                }
            }

            // M13a: art instances from live light state (the renderer
            // consumes below_/above_ between the scene draws).
            if (!art_renderer.build(
                    render_scene.lights.data(), render_scene.lights.size(), snap.sim_time_s)) {
                TB_LOG_WARN_RATELIMITED("main", "art instance budget exceeded; art truncated");
            }
            // .data() reads MEMBER vectors (stable, no temporaries).
            render::ArtInstances art_instances;
            const auto& below = art_renderer.below_ball();
            const auto& above = art_renderer.above_ball();
            art_instances.below = below.data();
            art_instances.below_count = uint32_t(below.size());
            art_instances.above = above.data();
            art_instances.above_count = uint32_t(above.size());
            frame.art = &art_instances;
            frame.lights = render_scene.lights.data();
            frame.light_count = uint32_t(render_scene.lights.size());

            frame.quads = quads.data();
            frame.quad_count = uint32_t(quads.size());
            frame.show_colliders = debug_level >= 1;
            frame.debug_colliders = render_scene.colliders.data();
            frame.debug_collider_count = uint32_t(render_scene.colliders.size());

            renderer->render_playfield(frame);

            // Backglass at ~30 Hz, non-blocking (07 §8): the attempt
            // cadence is deadline-driven; a skipped acquire retries
            // next playfield frame without advancing the deadline.
            if (backglass_window) {
                const uint64_t now_bg = tb_now_ns();
                if (bg_pacer.should_attempt(now_bg)) {
                    bg_built.clear();
                    // Everything below reads the SNAPSHOT — including
                    // the attract top-10 — no live game objects on this
                    // thread at all.
                    render::BackglassContent content;
                    content.in_attract = snap.game.player_count <= 0 ||
                                         snap.game.game_state == uint8_t(game::GameState::Attract);
                    // Attract page machine (§8.2) + the static table
                    // data the pages show (logo name, rules card).
                    content.attract_page = int(snap.game.attract_page);
                    content.attract_page_time_s = snap.game.attract_page_time_s;
                    if (loaded_table != nullptr) {
                        content.table_name = loaded_table->def.name;
                        // §8.2 rules card: meta.rules_card lines. The
                        // table object is immutable at runtime (loads
                        // and F5 reloads happen with the sim stopped).
                        std::istringstream card(loaded_table->def.rules_card);
                        std::string line;
                        while (std::getline(card, line)) {
                            if (!line.empty() && line.back() == '\r') {
                                line.pop_back();
                            }
                            content.rules_lines.push_back(line);
                            if (content.rules_lines.size() >= 8) {
                                break; // card cap; more is M15 polish
                            }
                        }
                    }
                    // Both bounds: >4 would write past content.scores,
                    // and the publisher-side clamp is a convention, not
                    // a type guarantee (cycle-26 review).
                    content.player_count =
                        std::clamp(snap.game.player_count, 1, decltype(snap.game)::kMaxPlayers);
                    content.current_player =
                        std::clamp(snap.game.current_player, 1, content.player_count);
                    content.ball_number = snap.game.ball_number;
                    for (int pi = 0; pi < content.player_count; ++pi) {
                        content.scores[size_t(pi)] = snap.game.scores[size_t(pi)];
                    }
                    // Attract top-10 from the snapshot copy — the
                    // table itself mutates on the same (sim) thread
                    // that fills the copy, never here.
                    content.high_score_count = std::min<uint32_t>(
                        snap.game.high_score_count, decltype(snap.game)::kHighScoreCap);
                    for (uint32_t i = 0; i < content.high_score_count; ++i) {
                        content.high_scores[i] = {{snap.game.high_scores[i].initials[0],
                                                   snap.game.high_scores[i].initials[1],
                                                   snap.game.high_scores[i].initials[2]},
                                                  snap.game.high_scores[i].score};
                    }
                    sim::BackglassModel model; // rebuilt from the snapshot copy
                    model.layout = snap.game.layout;
                    model.focus_player = snap.game.focus_player;
                    model.message_style = snap.game.message_style;
                    const uint32_t msg_len = uint32_t(
                        std::min(size_t(snap.game.message_len), sizeof(model.message) - 1));
                    model.message_len = msg_len;
                    std::memcpy(model.message, snap.game.message, msg_len);
                    model.message[msg_len] = '\0';
                    bg_layout.build(content, model, bg_font, &bg_built);
                    render::BackglassFrame bframe;
                    bframe.quads = bg_built.data();
                    bframe.quad_count = uint32_t(bg_built.size());
                    if (renderer->render_backglass(bframe)) {
                        bg_pacer.report_drawn(now_bg);
                    } else {
                        bg_pacer.report_skipped();
                    }
                }
            }

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
