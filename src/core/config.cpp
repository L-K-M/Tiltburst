#include "core/config.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tb {

namespace {

using json = nlohmann::ordered_json;

constexpr const char* kBindingKeys[10] = {
    "left_flipper",
    "right_flipper",
    "left_flipper_2",
    "right_flipper_2",
    "plunger",
    "nudge_left",
    "nudge_right",
    "nudge_up",
    "start",
    "pause",
};

std::array<std::vector<std::string>, 10> default_bindings() {
    return {
        std::vector<std::string>{"Left Shift"},
        {"Right Shift"},
        {"Left Shift"},
        {"Right Shift"},
        {"Space"},
        {"Z"},
        {"/"},
        {"X"},
        {"1", "Return"},
        {"Escape"},
    };
}

template <typename T>
T clamp_warn(T v, T lo, T hi, const char* key) {
    if (v < lo || v > hi) {
        TB_LOG_WARN("main", "{} value {} out of range [{}, {}]; clamped", key, v, lo, hi);
        return v < lo ? lo : hi;
    }
    return v;
}

void read_bindings(const json& in, Settings& s) {
    for (int i = 0; i < 10; ++i) {
        auto it = in.find(kBindingKeys[i]);
        if (it == in.end() || !it->is_array()) {
            continue;
        }
        s.bindings[i].clear();
        for (const auto& name : *it) {
            if (name.is_string()) {
                s.bindings[i].push_back(name.get<std::string>());
            }
        }
    }
}

void write_bindings(json& out, const Settings& s) {
    for (int i = 0; i < 10; ++i) {
        out[kBindingKeys[i]] = s.bindings[i];
    }
}

} // namespace

Settings Settings::defaults() {
    Settings s;
    s.bindings = default_bindings();
    return s;
}

Settings Settings::load(const std::filesystem::path& path) {
    Settings s = defaults();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return s;
    }

    json doc;
    try {
        std::ifstream in(path);
        // Canon §5.5: JSON may contain // comments.
        doc = json::parse(in,
                          /*callback=*/nullptr,
                          /*allow_exceptions=*/true,
                          /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        std::error_code rename_ec;
        std::filesystem::rename(path, path.string() + ".bad", rename_ec);
        TB_LOG_WARN("main",
                    "settings.json parse failed ({}); moved to "
                    "settings.json.bad, using defaults",
                    e.what());
        return s;
    }

    if (auto v = doc.find("video"); v != doc.end()) {
        if (auto k = v->find("present_mode"); k != v->end() && k->is_string()) {
            s.present_mode = k->get<std::string>();
        }
        if (auto k = v->find("max_fps"); k != v->end() && k->is_number_integer()) {
            const int raw = k->get<int>();
            if (raw == -1 || raw == 0) {
                s.max_fps = raw; // sentinels are never clamped (§11.1)
            } else {
                s.max_fps = clamp_warn(raw, 30, 1000, "video.max_fps");
            }
        }
        if (auto k = v->find("brightness"); k != v->end() && k->is_number()) {
            s.brightness = clamp_warn(k->get<float>(), 0.5f, 1.5f, "video.brightness");
        }
    }

    if (auto v = doc.find("render"); v != doc.end()) {
        auto read_bool = [&](const char* key, bool& out) {
            if (auto k = v->find(key); k != v->end() && k->is_boolean()) {
                out = k->get<bool>();
            }
        };
        auto read_num = [&](const char* key, float& out) {
            if (auto k = v->find(key); k != v->end() && k->is_number()) {
                out = k->get<float>();
            }
        };
        read_bool("bloom_enabled", s.bloom_enabled);
        read_num("bloom_threshold", s.bloom_threshold);
        read_num("bloom_knee", s.bloom_knee);
        read_num("bloom_strength", s.bloom_strength);
        read_bool("crt", s.crt);
    }

    if (auto v = doc.find("audio"); v != doc.end()) {
        auto read_vol = [&](const char* key, int& out) {
            if (auto k = v->find(key); k != v->end() && k->is_number_integer()) {
                out = clamp_warn(k->get<int>(), 0, 100, key);
            }
        };
        read_vol("master", s.audio_master);
        read_vol("sfx", s.audio_sfx);
        read_vol("music", s.audio_music);
        read_vol("ui", s.audio_ui);
        if (auto k = v->find("period_frames"); k != v->end() && k->is_number_integer()) {
            s.audio_period_frames = k->get<int>();
        }
    }

    if (auto v = doc.find("input"); v != doc.end()) {
        read_bindings(*v, s);
        if (auto k = v->find("plunger_max_pull_s"); k != v->end() && k->is_number()) {
            s.plunger_max_pull_s = clamp_warn(k->get<float>(), 0.5f, 3.0f, "plunger_max_pull_s");
        }
        if (auto k = v->find("nudge_level"); k != v->end() && k->is_number_integer()) {
            const int raw = k->get<int>();
            if (raw >= 1 && raw <= 3) {
                s.nudge_level = raw;
            } else {
                TB_LOG_WARN("main", "nudge_level {} not in {{1,2,3}}; using 2", raw);
                s.nudge_level = 2;
            }
        }
    }

    if (auto v = doc.find("game"); v != doc.end()) {
        if (auto k = v->find("balls_per_game"); k != v->end() && k->is_number_integer()) {
            const int raw = k->get<int>();
            if (raw == 3 || raw == 5) {
                s.balls_per_game = raw;
            } else {
                TB_LOG_WARN("main",
                            "balls_per_game {} is neither 3 nor 5; "
                            "using 3",
                            raw);
                s.balls_per_game = 3;
            }
        }
        if (auto k = v->find("tilt_warnings"); k != v->end() && k->is_number_integer()) {
            s.tilt_warnings = clamp_warn(k->get<int>(), 1, 3, "tilt_warnings");
        }
        if (auto k = v->find("ball_save_seconds"); k != v->end() && k->is_number_integer()) {
            s.ball_save_seconds = clamp_warn(k->get<int>(), 0, 15, "ball_save_seconds");
        }
        if (auto k = v->find("replay_award"); k != v->end() && k->is_string()) {
            s.replay_award = k->get<std::string>();
        }
    }

    if (auto v = doc.find("accessibility"); v != doc.end()) {
        auto read_bool = [&](const char* key, bool& out) {
            if (auto k = v->find(key); k != v->end() && k->is_boolean()) {
                out = k->get<bool>();
            }
        };
        read_bool("reduce_flashing", s.reduce_flashing);
        read_bool("ball_outline", s.ball_outline);
        read_bool("screen_shake", s.screen_shake);
    }

    if (auto k = doc.find("last_table"); k != doc.end() && k->is_string()) {
        s.last_table = k->get<std::string>();
    }

    return s;
}

bool Settings::save(const std::filesystem::path& path) const {
    // Preserve unknown keys: re-read the current document and overwrite
    // known leaves (§11.1). Missing file starts from an empty object.
    json doc = json::object();
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            try {
                doc = json::parse(in, nullptr, true, true);
            } catch (const std::exception&) {
                doc = json::object();
            }
            if (!doc.is_object()) {
                doc = json::object();
            }
        }
    }

    auto& video = doc["video"];
    video["present_mode"] = present_mode;
    video["max_fps"] = max_fps;
    video["brightness"] = brightness;

    auto& render = doc["render"];
    render["bloom_enabled"] = bloom_enabled;
    render["bloom_threshold"] = bloom_threshold;
    render["bloom_knee"] = bloom_knee;
    render["bloom_strength"] = bloom_strength;
    render["crt"] = crt;

    auto& audio = doc["audio"];
    audio["master"] = audio_master;
    audio["sfx"] = audio_sfx;
    audio["music"] = audio_music;
    audio["ui"] = audio_ui;
    audio["period_frames"] = audio_period_frames;

    auto& input = doc["input"];
    write_bindings(input, *this);
    input["plunger_max_pull_s"] = plunger_max_pull_s;
    input["nudge_level"] = nudge_level;

    auto& game = doc["game"];
    game["balls_per_game"] = balls_per_game;
    game["tilt_warnings"] = tilt_warnings;
    game["ball_save_seconds"] = ball_save_seconds;
    game["replay_award"] = replay_award;

    auto& access = doc["accessibility"];
    access["reduce_flashing"] = reduce_flashing;
    access["ball_outline"] = ball_outline;
    access["screen_shake"] = screen_shake;

    doc["last_table"] = last_table;

    // Crash-safe write (§11.2): tmp + fsync + rename (+ dir fsync on POSIX).
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
        if (!f) {
            TB_LOG_ERROR("main", "settings save: cannot open {}", tmp.string());
            return false;
        }
        const std::string text = doc.dump(2) + "\n";
        std::fwrite(text.data(), 1, text.size(), f);
        std::fflush(f);
#if defined(_WIN32)
        _commit(_fileno(f));
#else
        fsync(fileno(f));
#endif
        std::fclose(f);
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        TB_LOG_ERROR("main", "settings save: rename failed ({})", ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }

#if !defined(_WIN32)
    if (int dirfd = ::open(path.parent_path().string().c_str(), O_RDONLY); dirfd >= 0) {
        ::fsync(dirfd);
        ::close(dirfd);
    }
#endif
    return true;
}

} // namespace tb
