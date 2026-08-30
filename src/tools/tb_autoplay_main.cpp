// tb_autoplay (14-authoring-guide.md §8.2 — the normative contract).
// Headless deterministic simulation: scripted plunge + skill-profiled
// flipper policy; two session shapes (balls3 / seconds300); report per
// the §8.2 schema; --check-bounds verdicts per 16 §2.8 applicability.
//
// v1 approximations (journaled): shot attempts ride the aiming policy
// (skill 0 does not aim, so its shot rates are made/made); nudge logic
// is not modeled (tilts stay 0 beyond framework-driven ones); ball
// saves use the serve-after-drain-same-ball heuristic.
#include "core/rng.h"
#include "game/game_machine.h"
#include "sim/music_sink.h"
#include "sim/script_host.h"
#include "sim/solver.h"
#include "table/sim_builder.h"
#include "table/table_loader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace tb;
using nlohmann::ordered_json;
using sim::Vec2;

namespace {

constexpr const char* kUsage = "usage: tb_autoplay <table-dir> --runs N --skill {0|1|2} --seed S\n"
                               "       [--balls 3 | --seconds 300] [--report out.json]\n"
                               "       [--check-bounds] [--replay tape.replay.json]\n"
                               "       [--record-golden out.hashes]\n";

struct Cli {
    std::filesystem::path dir;
    int runs = 1;
    int skill = 1;
    uint64_t seed = 0;
    int balls = 3;
    uint64_t seconds = 0; // 0 => balls3 shape
    std::filesystem::path report, replay, golden;
    bool check_bounds = false;
};

bool parse_cli(int argc, char** argv, Cli& cli, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                err = std::string(what) + " requires a value";
                return "";
            }
            return argv[++i];
        };
        if (a == "--runs") {
            cli.runs = std::atoi(next("--runs").c_str());
        } else if (a == "--skill") {
            cli.skill = std::atoi(next("--skill").c_str());
        } else if (a == "--seed") {
            cli.seed = uint64_t(std::atoll(next("--seed").c_str()));
        } else if (a == "--balls") {
            cli.balls = std::atoi(next("--balls").c_str());
        } else if (a == "--seconds") {
            cli.seconds = uint64_t(std::atoll(next("--seconds").c_str()));
        } else if (a == "--report") {
            cli.report = next("--report");
        } else if (a == "--replay") {
            cli.replay = next("--replay");
        } else if (a == "--record-golden") {
            cli.golden = next("--record-golden");
        } else if (a == "--check-bounds") {
            cli.check_bounds = true;
        } else if (!a.empty() && a[0] == '-') {
            err = "unknown flag: " + a;
            return false;
        } else if (cli.dir.empty()) {
            cli.dir = a;
        } else {
            err = "unexpected argument: " + a;
            return false;
        }
    }
    if (cli.dir.empty()) {
        err = "table directory required";
        return false;
    }
    if (cli.skill < 0 || cli.skill > 2) {
        err = "--skill must be 0, 1, or 2";
        return false;
    }
    if (cli.runs < 1) {
        err = "--runs must be >= 1";
        return false;
    }
    if (cli.balls < 1 || cli.balls > 5) {
        err = "--balls must be 1..5";
        return false;
    }
    if (!cli.golden.empty() && cli.replay.empty()) {
        err = "--record-golden requires --replay";
        return false;
    }
    return true;
}

// ---- skill profile (14 §8.2) ----
struct SkillProfile {
    double delay_ms = 120;
    double sigma_ms = 60;
    bool aiming = false;
};

SkillProfile skill_profile(int s) {
    switch (s) {
    case 2:
        return {50, 10, true};
    case 1:
        return {80, 25, true};
    default:
        return {120, 60, false};
    }
}

// ---- report accumulators (14 §8.2 schema) ----
struct Shot {
    uint64_t attempts = 0, made = 0;
};

struct Report {
    std::string shape;
    int balls = 0;
    uint64_t script_errors = 0;
    uint64_t stuck_balls = 0;
    std::vector<double> ball_times;
    uint64_t drains_center = 0, drains_left = 0, drains_right = 0;
    std::set<std::string> covered;
    size_t scoring_total = 0;
    std::map<std::string, Shot> shots;
    std::vector<uint64_t> scores;
    std::vector<std::vector<uint64_t>> per_ball;
    uint64_t mode_starts = 0, mode_ends = 0, multiballs = 0, wizards = 0;
    uint64_t games = 0;
    uint64_t flips = 0, balls_played = 0;
    uint64_t tilt_warnings = 0, tilts = 0, ball_saves = 0;
};

double pctl(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

// Mode/wizard/multiball detection rides the §9 music seam: the script
// and framework announce state changes through play_music.
struct MusicRecorder : sim::MusicSink {
    std::string current;
    Report* rep = nullptr;

    void play_music(const char* song_id) override {
        if (song_id == nullptr) {
            return;
        }
        const std::string next = song_id;
        if (next == "mode" && current != "mode") {
            ++rep->mode_starts;
        }
        if (current == "mode" && next != "mode") {
            ++rep->mode_ends;
        }
        if (next == "multiball" && current != "multiball") {
            ++rep->multiballs;
        }
        if (next == "wizard" && current != "wizard") {
            ++rep->wizards;
        }
        current = next;
    }

    void stop_music() override { current.clear(); }
};

// ---- replay tape (16 §2.4.2) ----
struct Tape {
    uint64_t seed = 0;
    std::vector<uint32_t> masks;
    bool loaded = false;
};

bool load_tape(const std::filesystem::path& p, Tape& out) {
    std::ifstream in(p);
    if (!in.good()) {
        return false;
    }
    try {
        ordered_json j = ordered_json::parse(in, nullptr, true, true);
        out.seed = j.value("seed", uint64_t(0));
        out.masks.clear();
        if (j.contains("inputs") && j.at("inputs").is_array()) {
            for (const auto& e : j.at("inputs")) {
                out.masks.push_back(e.value("mask", uint32_t(0)));
            }
        }
        out.loaded = true;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// 05 §13.1 edge mapping: bits 0-4 and 6-9 are 1:1; bit 5 ORs into 4.
uint32_t tape_mask_to_buttons(uint32_t mask) {
    uint32_t b = mask & 0x33F;
    if (mask & (1u << 5)) {
        b |= 1u << 4;
    }
    return b;
}

struct PolicyFlipper {
    float wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;
    bool left = true;
    uint32_t bit = 0;
    int64_t fire_at_tick = -1;
    uint32_t hold_until = 0;
};

bool is_scoring_event(sim::SimEventType t) {
    switch (t) {
    case sim::SimEventType::SwitchHit:
    case sim::SimEventType::SpinnerSpin:
    case sim::SimEventType::RolloverEvent:
    case sim::SimEventType::KickerEnter:
    case sim::SimEventType::TargetDown:
    case sim::SimEventType::BankComplete:
    case sim::SimEventType::CaptiveFullTravel:
    case sim::SimEventType::RampMade:
    case sim::SimEventType::BallLockCapture:
        return true;
    default:
        return false;
    }
}

struct RunContext {
    std::set<std::string> scoring_ids;
    std::vector<std::string> shot_ids;
    std::set<std::string> shot_set;
};

// The per-run loop, shared by both session shapes.
void run_one(const Cli& cli,
             const table::TableDef& def,
             const Tape& tape,
             Report& rep,
             const SkillProfile& prof,
             Pcg32& rng,
             const std::function<double()>& gauss,
             const std::string& rules,
             const RunContext& ctx,
             std::vector<uint64_t>& golden_out) {
    const bool balls_shape = cli.seconds == 0;

    sim::SimState s;
    table::build_sim(def, s);
    sim::Solver solver;
    sim::ScriptHost host;
    s.script = &host;
    host.load(rules, s);

    MusicRecorder music;
    music.rep = &rep;
    host.set_music_sink(&music);

    std::unique_ptr<game::GameMachine> machine;
    if (balls_shape) {
        game::FrameworkConfig fcfg;
        fcfg.balls_per_game = cli.balls;
        game::HighScoreTable hst;
        machine = std::make_unique<game::GameMachine>(host, s, hst, fcfg);
        machine->set_music_sink(&music);
        s.fsm_ctx = machine.get();
        s.fsm_step = [](void* ctx, sim::SimState&, const sim::TickInput& in) {
            static_cast<game::GameMachine*>(ctx)->step(in);
        };
    } else {
        // Continuous session: no framework — fsm_step stays null so the
        // solver's auto-serve owns instant respawns (§8.2).
        s.fsm_step = nullptr;
        host.begin_game(1);
    }

    std::vector<PolicyFlipper> flippers;
    for (const sim::Flipper& f : s.flippers) {
        if (f.params.action > 1) {
            continue; // lower pair only
        }
        PolicyFlipper pf;
        pf.left = f.params.side_sign > 0;
        pf.bit = f.params.action == 0 ? 0u : 1u;
        const float len = f.params.length + f.params.radius_base;
        pf.wx0 = f.params.pivot.x - len;
        pf.wx1 = f.params.pivot.x + len;
        pf.wy0 = f.params.pivot.y - f.params.radius_base;
        pf.wy1 = f.params.pivot.y + len + 0.060f; // +60 mm up-table
        flippers.push_back(pf);
    }

    uint64_t ticks = 0;
    const uint64_t max_ticks = balls_shape ? uint64_t(3'600'000) : cli.seconds * 1000;
    bool ball_up = false;
    uint64_t ball_start_tick = 0;
    bool plunge_armed = false;
    uint64_t plunge_release = 0;
    std::string aim_target;
    Vec2 stuck_ref{};
    uint64_t stuck_since = 0;
    int prev_ball_number = 1;
    uint64_t last_drain_tick = 0;
    std::vector<uint64_t> per_ball_scores;
    bool tilted_this_game = false;

    while (ticks < max_ticks) {
        uint32_t buttons = 0;
        if (tape.loaded) {
            buttons =
                ticks < tape.masks.size() ? tape_mask_to_buttons(tape.masks[size_t(ticks)]) : 0;
        } else {
            // Scripted plunge: charge while a ball rests in the
            // plunger zone (the sim's own zone flag — a geometric
            // y/velocity probe flickers during compression and never
            // releases), then release to fire.
            const bool ball_on_plunger = s.plunger.ball_in_zone;
            if (ball_on_plunger && !plunge_armed) {
                plunge_armed = true;
                plunge_release = ticks + 900 + uint64_t(rng.next_u32() % 400u);
            }
            if (!ball_on_plunger) {
                plunge_armed = false;
            }
            if (plunge_armed && ticks < plunge_release) {
                buttons |= 1u << 4;
            }

            // Flip policy: window entry moving down-table (§8.2).
            for (const sim::Ball& b : s.balls) {
                if (!b.live || b.mode != sim::BallMode::Free || b.vel.y >= 0.0f) {
                    continue;
                }
                for (PolicyFlipper& pf : flippers) {
                    if (pf.fire_at_tick >= 0) {
                        continue;
                    }
                    if (b.pos.x < pf.wx0 || b.pos.x > pf.wx1 || b.pos.y < pf.wy0 ||
                        b.pos.y > pf.wy1) {
                        continue;
                    }
                    if (prof.aiming && aim_target.empty() && !ctx.shot_ids.empty()) {
                        aim_target = ctx.shot_ids[rng.next_u32() % uint32_t(ctx.shot_ids.size())];
                    }
                    bool fire = true;
                    if (prof.aiming && !aim_target.empty()) {
                        // Target's entry x picks the flipper side.
                        const float tx = [&] {
                            for (const auto& e : def.elements) {
                                if (e.id() == aim_target &&
                                    std::holds_alternative<table::RampDef>(e.def)) {
                                    const auto& p = std::get<table::RampDef>(e.def).path;
                                    if (!p.empty()) {
                                        return p.front().point[0];
                                    }
                                }
                            }
                            return def.width * 0.5f;
                        }();
                        fire = (tx < def.width * 0.5f) == pf.left;
                        if (fire) {
                            rep.shots[aim_target].attempts++;
                            aim_target.clear();
                        }
                    }
                    if (fire) {
                        const double dms = prof.delay_ms + gauss() * prof.sigma_ms;
                        pf.fire_at_tick = int64_t(ticks) + int64_t(std::max(0.0, dms));
                    }
                }
            }
            for (PolicyFlipper& pf : flippers) {
                if (pf.fire_at_tick >= 0 && int64_t(ticks) >= pf.fire_at_tick) {
                    pf.hold_until = uint32_t(ticks + 60);
                    pf.fire_at_tick = -1;
                    ++rep.flips;
                }
                if (pf.hold_until > ticks) {
                    buttons |= 1u << pf.bit;
                }
            }
        }

        solver.step(s, sim::TickInput{buttons});
        ++ticks;
        // Observe the tick's event log (phase 2 dispatched to the
        // script inside step; the log persists for this read).
        for (size_t i = 0; i < s.tick_event_n; ++i) {
            const sim::SimEvent& ev = s.tick_events[i];
            const std::string id =
                ev.element < s.element_ids.size() ? s.element_ids[ev.element] : "";
            if (is_scoring_event(sim::SimEventType(ev.type))) {
                if (ctx.scoring_ids.count(id) != 0) {
                    rep.covered.insert(id);
                }
                if (ctx.shot_set.count(id) != 0 &&
                    ev.type != uint16_t(sim::SimEventType::RampMade)) {
                    rep.shots[id].made++; // loop switch hits
                }
            }
            if (sim::SimEventType(ev.type) == sim::SimEventType::RampMade &&
                ctx.shot_set.count(id) != 0) {
                rep.shots[id].made++;
            }
            if (sim::SimEventType(ev.type) == sim::SimEventType::Drain) {
                if (ev.x < def.width * 0.20f) {
                    ++rep.drains_left;
                } else if (ev.x > def.width * 0.80f) {
                    ++rep.drains_right;
                } else {
                    ++rep.drains_center;
                }
                last_drain_tick = ticks;
            }
            if (sim::SimEventType(ev.type) == sim::SimEventType::DangerThreshold) {
                ++rep.tilt_warnings;
            }
        }

        bool any_free = false;
        for (const sim::Ball& b : s.balls) {
            any_free = any_free || (b.live && b.mode == sim::BallMode::Free);
        }
        if (!ball_up && any_free) {
            ball_up = true;
            ball_start_tick = ticks;
            ++rep.balls_played;
            stuck_ref = {0, 0};
            stuck_since = ticks;
            if (balls_shape && last_drain_tick > 0 && ticks - last_drain_tick < 1500 &&
                machine->player(machine->current_player()).ball_number == prev_ball_number) {
                ++rep.ball_saves; // serve-after-drain, same ball (v1)
            }
            if (balls_shape) {
                prev_ball_number = machine->player(machine->current_player()).ball_number;
            }
        }
        if (ball_up && !any_free) {
            ball_up = false;
            rep.ball_times.push_back(double(ticks - ball_start_tick) / 1000.0);
            if (balls_shape) {
                per_ball_scores.push_back(host.player_scores(1).score);
            }
        }

        // Stuck detection (§8.2: within 27 mm for 10 s, not held).
        if (any_free) {
            for (const sim::Ball& b : s.balls) {
                if (!b.live || b.mode != sim::BallMode::Free) {
                    continue;
                }
                const float d = std::hypot(b.pos.x - stuck_ref.x, b.pos.y - stuck_ref.y);
                if (d > 0.027f) {
                    stuck_ref = b.pos;
                    stuck_since = ticks;
                } else if (ticks - stuck_since > 10'000) {
                    ++rep.stuck_balls;
                    for (sim::Ball& db : s.balls) {
                        if (db.live && db.mode == sim::BallMode::Free) {
                            db.live = false; // respawn via the server
                        }
                    }
                    stuck_since = ticks;
                }
                break; // watch the first free ball only
            }
        }

        if (ticks % 1000 == 0) {
            golden_out.push_back(sim::state_hash(s));
        }

        if (balls_shape) {
            if (machine->tilted() && !tilted_this_game) {
                tilted_this_game = true;
                ++rep.tilts;
            }
            if (machine->state() == game::GameState::GameOver) {
                break;
            }
        }
    }

    if (balls_shape && machine != nullptr) {
        ++rep.games;
        uint64_t total = 0;
        for (int p = 1; p <= std::max(1, machine->player_count()); ++p) {
            total += host.player_scores(p).score;
        }
        rep.scores.push_back(total);
        rep.per_ball.push_back(per_ball_scores);
    }
    host.set_music_sink(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    Cli cli;
    std::string err;
    if (!parse_cli(argc, argv, cli, err)) {
        std::fprintf(stderr, "%s\n%s", err.c_str(), kUsage);
        return 2;
    }
    if (!std::filesystem::is_directory(cli.dir)) {
        std::fprintf(stderr, "not a directory: %s\n", cli.dir.string().c_str());
        return 2;
    }

    table::TableDef def;
    try {
        def = table::load_table(cli.dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "table load failed: %s\n", e.what());
        return 1;
    }

    Tape tape;
    if (!cli.replay.empty() && !load_tape(cli.replay, tape)) {
        std::fprintf(stderr, "cannot load replay tape: %s\n", cli.replay.string().c_str());
        return 2;
    }

    Report rep;
    RunContext ctx;
    for (const auto& e : def.elements) {
        const bool scoring = std::holds_alternative<table::RolloverDef>(e.def) ||
                             std::holds_alternative<table::SpinnerDef>(e.def) ||
                             std::holds_alternative<table::KickerDef>(e.def) ||
                             std::holds_alternative<table::DropTargetBankDef>(e.def) ||
                             std::holds_alternative<table::PopBumperDef>(e.def) ||
                             std::holds_alternative<table::StandupTargetDef>(e.def) ||
                             std::holds_alternative<table::SlingshotDef>(e.def) ||
                             std::holds_alternative<table::RampDef>(e.def);
        if (scoring) {
            ctx.scoring_ids.insert(e.id());
        }
        if (std::holds_alternative<table::RampDef>(e.def)) {
            ctx.shot_ids.push_back(e.id());
            ctx.shot_set.insert(e.id());
        }
        if (std::holds_alternative<table::GateDef>(e.def) &&
            e.id().find("_switch") != std::string::npos) {
            ctx.shot_ids.push_back(e.id());
            ctx.shot_set.insert(e.id());
        }
    }
    rep.scoring_total = ctx.scoring_ids.size();
    rep.shape = cli.seconds == 0 ? "balls3" : "seconds300";
    rep.balls = cli.balls;

    std::ifstream rl(cli.dir / "rules.lua");
    std::ostringstream rss;
    rss << rl.rdbuf();
    const std::string rules = rss.str();

    std::vector<uint64_t> golden;
    for (int run = 0; run < cli.runs; ++run) {
        Pcg32 rng;
        rng.seed(tape.loaded ? tape.seed + uint64_t(run) : cli.seed + uint64_t(run), 0xA5A5A5A5ull);
        const auto gauss = [&rng]() {
            const double u1 = std::max(1e-9, double(rng.next_float()));
            const double u2 = double(rng.next_float());
            return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        };
        run_one(cli, def, tape, rep, skill_profile(cli.skill), rng, gauss, rules, ctx, golden);
    }

    // ---- report (14 §8.2 schema) ----
    const bool gs = rep.shape == "balls3";
    ordered_json j;
    j["table"] = def.slug;
    j["runs"] = cli.runs;
    j["skill"] = cli.skill;
    j["seed"] = cli.seed;
    j["shape"] = rep.shape;
    j["balls"] = gs ? cli.balls : 0;
    j["script_errors"] = rep.script_errors;
    j["stuck_balls"] = rep.stuck_balls;
    {
        ordered_json bt;
        bt["p10"] = pctl(rep.ball_times, 0.10);
        bt["p50"] = pctl(rep.ball_times, 0.50);
        bt["p90"] = pctl(rep.ball_times, 0.90);
        j["ball_time_s"] = bt;
    }
    {
        const uint64_t total = rep.drains_center + rep.drains_left + rep.drains_right;
        const auto share = [&](uint64_t n) { return total > 0 ? double(n) / double(total) : 0.0; };
        ordered_json dr;
        dr["center"] = share(rep.drains_center);
        dr["left_outlane"] = share(rep.drains_left);
        dr["right_outlane"] = share(rep.drains_right);
        dr["outlane_share"] = share(rep.drains_left + rep.drains_right);
        j["drains"] = dr;
    }
    {
        ordered_json cov;
        cov["hit"] = rep.covered.size();
        cov["total"] = rep.scoring_total;
        cov["share"] =
            rep.scoring_total > 0 ? double(rep.covered.size()) / double(rep.scoring_total) : 0.0;
        ordered_json missed = ordered_json::array();
        for (const std::string& id : ctx.scoring_ids) {
            if (rep.covered.count(id) == 0) {
                missed.push_back(id);
            }
        }
        cov["missed"] = missed;
        j["coverage"] = cov;
    }
    {
        ordered_json sh = ordered_json::array();
        for (const auto& [id, s] : rep.shots) {
            ordered_json o;
            o["id"] = id;
            o["attempts"] = s.attempts;
            o["made"] = s.made;
            o["rate"] = s.attempts > 0 ? double(s.made) / double(s.attempts) : 0.0;
            sh.push_back(o);
        }
        j["shots"] = sh;
    }
    {
        ordered_json sc;
        if (gs) {
            std::vector<double> sv;
            for (uint64_t v : rep.scores) {
                sv.push_back(double(v));
            }
            sc["p10"] = pctl(sv, 0.10);
            sc["p50"] = pctl(sv, 0.50);
            sc["p90"] = pctl(sv, 0.90);
            ordered_json pba = ordered_json::array();
            for (size_t b = 0; b < size_t(cli.balls); ++b) {
                std::vector<double> col;
                for (const auto& g : rep.per_ball) {
                    if (b < g.size()) {
                        col.push_back(double(g[b]));
                    }
                }
                pba.push_back(pctl(col, 0.50));
            }
            sc["per_ball_p50"] = pba;
        } else {
            sc["p10"] = nullptr;
            sc["p50"] = nullptr;
            sc["p90"] = nullptr;
            sc["per_ball_p50"] = nullptr;
        }
        j["score"] = sc;
    }
    {
        ordered_json mo;
        if (gs) {
            const double g = rep.games > 0 ? double(rep.games) : 1.0;
            mo["started_per_game"] = double(rep.mode_starts) / g;
            mo["completed_per_game"] = double(rep.mode_ends) / g;
            mo["multiball_reach_share"] = double(rep.multiballs) / g;
            mo["wizard_reach_share"] = double(rep.wizards) / g;
        } else {
            mo["started_per_game"] = nullptr;
            mo["completed_per_game"] = nullptr;
            mo["multiball_reach_share"] = nullptr;
            mo["wizard_reach_share"] = nullptr;
        }
        j["modes"] = mo;
    }
    j["flips_per_ball"] = rep.balls_played > 0 ? double(rep.flips) / double(rep.balls_played) : 0;
    if (gs) {
        const double g = rep.games > 0 ? double(rep.games) : 1.0;
        j["tilt_warnings_per_game"] = double(rep.tilt_warnings) / g;
        j["tilts"] = rep.tilts;
        j["ball_saves_used_per_game"] = double(rep.ball_saves) / g;
    } else {
        j["tilt_warnings_per_game"] = nullptr;
        j["tilts"] = rep.tilts;
        j["ball_saves_used_per_game"] = nullptr;
    }

    if (!cli.report.empty()) {
        std::ofstream out(cli.report);
        out << j.dump(2) << "\n";
    } else {
        std::printf("%s\n", j.dump(2).c_str());
    }
    if (!cli.golden.empty()) {
        std::ofstream out(cli.golden);
        char buf[24];
        for (uint64_t h : golden) {
            std::snprintf(buf, sizeof(buf), "%016llx\n", (unsigned long long)h);
            out << buf;
        }
    }

    // ---- --check-bounds verdict (16 §2.8) ----
    if (cli.check_bounds) {
        bool ok = rep.stuck_balls == 0 && rep.script_errors == 0;
        std::ifstream tj(cli.dir / "table.json");
        try {
            ordered_json doc = ordered_json::parse(tj, nullptr, true, true);
            if (doc.contains("meta") && doc.at("meta").contains("autoplay_bounds")) {
                for (auto it = doc.at("meta").at("autoplay_bounds").begin();
                     it != doc.at("meta").at("autoplay_bounds").end();
                     ++it) {
                    if (!it->contains("skill") || it->at("skill").get<int>() != cli.skill) {
                        continue; // not applicable at this skill
                    }
                    double value = 0;
                    bool resolved = false;
                    const std::string path = it.key();
                    if (path.rfind("shots[", 0) == 0) {
                        const size_t close = path.find(']');
                        const std::string id = path.substr(6, close - 6);
                        const std::string field = path.substr(close + 2);
                        for (const auto& so : j.at("shots")) {
                            if (so.at("id").get<std::string>() == id && so.at(field).is_number()) {
                                value = so.at(field).get<double>();
                                resolved = true;
                            }
                        }
                    } else {
                        const ordered_json* node = &j;
                        resolved = true;
                        std::stringstream ss(path);
                        std::string item;
                        while (std::getline(ss, item, '.')) {
                            if (node->contains(item)) {
                                node = &node->at(item);
                            } else {
                                resolved = false;
                            }
                        }
                        if (resolved && node->is_number()) {
                            value = node->get<double>();
                        } else {
                            resolved = false; // null for this shape: skip
                        }
                    }
                    if (!resolved) {
                        continue;
                    }
                    if (it->contains("min") && value < it->at("min").get<double>()) {
                        std::fprintf(stderr,
                                     "bounds violation: %s = %.4f < min %.4f\n",
                                     path.c_str(),
                                     value,
                                     it->at("min").get<double>());
                        ok = false;
                    }
                    if (it->contains("max") && value > it->at("max").get<double>()) {
                        std::fprintf(stderr,
                                     "bounds violation: %s = %.4f > max %.4f\n",
                                     path.c_str(),
                                     value,
                                     it->at("max").get<double>());
                        ok = false;
                    }
                }
            }
        } catch (const std::exception&) {
            // No bounds: the stuck/error verdicts still apply.
        }
        return ok ? 0 : 1;
    }
    return 0;
}
