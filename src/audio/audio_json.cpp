#include "audio/audio_json.h"

#include "core/log.h"
#include "miniaudio.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tb::audio {

using json = nlohmann::ordered_json; // §5.5: ids 24+ follow JSON key order

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& pointer) {
    throw AudioLoadError("audio.json: " + what, pointer);
}

float get_param(
    const json& obj, const char* key, float def, float lo, float hi, const std::string& pointer) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (!it->is_number()) {
        fail(std::string("'") + key + "' must be a number", pointer + "/" + key);
    }
    const double v = it->get<double>();
    if (!(v >= double(lo) && v <= double(hi))) {
        fail(std::string("'") + key + "' outside " + std::to_string(lo) + ".." + std::to_string(hi),
             pointer + "/" + key);
    }
    return float(v);
}

Wave get_wave(const json& obj, const std::string& pointer) {
    const auto it = obj.find("wave");
    if (it == obj.end()) {
        return Wave::Square;
    }
    if (!it->is_string()) {
        fail("'wave' must be a string", pointer + "/wave");
    }
    const std::string w = it->get<std::string>();
    if (w == "square")
        return Wave::Square;
    if (w == "saw")
        return Wave::Saw;
    if (w == "sine")
        return Wave::Sine;
    if (w == "noise")
        return Wave::Noise;
    if (w == "triangle")
        return Wave::Triangle;
    fail("unknown wave '" + w + "'", pointer + "/wave");
}

// The complete §5.1 key set; anything else is an error.
bool is_known_key(const std::string& k) {
    static const char* const kKeys[] = {
        "wave",
        "duty",
        "duty_sweep",
        "attack",
        "sustain",
        "punch",
        "decay",
        "base_freq",
        "freq_limit",
        "freq_slide",
        "freq_delta_slide",
        "vib_depth",
        "vib_speed",
        "arp_mod",
        "arp_speed",
        "repeat_speed",
        "flanger_offset",
        "flanger_sweep",
        "lpf_cutoff",
        "lpf_sweep",
        "lpf_resonance",
        "hpf_cutoff",
        "hpf_sweep",
        "volume_db",
        "priority",
    };
    for (const char* known : kKeys) {
        if (k == known) {
            return true;
        }
    }
    return false;
}

} // namespace

SfxPatch parse_patch_json(const json& obj, const std::string& pointer) {
    if (!obj.is_object()) {
        fail("patch must be an object", pointer);
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!is_known_key(it.key())) {
            fail("unknown patch key '" + it.key() + "'", pointer + "/" + it.key());
        }
    }
    SfxPatch p;
    p.wave = get_wave(obj, pointer);
    p.duty = get_param(obj, "duty", p.duty, 0.02f, 0.98f, pointer);
    p.duty_sweep = get_param(obj, "duty_sweep", p.duty_sweep, -1.0f, 1.0f, pointer);
    p.attack = get_param(obj, "attack", p.attack, 0.0f, 1.0f, pointer);
    p.sustain = get_param(obj, "sustain", p.sustain, 0.0f, 1.0f, pointer);
    p.punch = get_param(obj, "punch", p.punch, 0.0f, 1.0f, pointer);
    p.decay = get_param(obj, "decay", p.decay, 0.0f, 1.0f, pointer);
    p.base_freq = get_param(obj, "base_freq", p.base_freq, 0.0f, 1.0f, pointer);
    p.freq_limit = get_param(obj, "freq_limit", p.freq_limit, 0.0f, 1.0f, pointer);
    p.freq_slide = get_param(obj, "freq_slide", p.freq_slide, -1.0f, 1.0f, pointer);
    p.freq_delta_slide =
        get_param(obj, "freq_delta_slide", p.freq_delta_slide, -1.0f, 1.0f, pointer);
    p.vib_depth = get_param(obj, "vib_depth", p.vib_depth, 0.0f, 1.0f, pointer);
    p.vib_speed = get_param(obj, "vib_speed", p.vib_speed, 0.0f, 1.0f, pointer);
    p.arp_mod = get_param(obj, "arp_mod", p.arp_mod, -1.0f, 1.0f, pointer);
    p.arp_speed = get_param(obj, "arp_speed", p.arp_speed, 0.0f, 1.0f, pointer);
    p.repeat_speed = get_param(obj, "repeat_speed", p.repeat_speed, 0.0f, 1.0f, pointer);
    p.flanger_offset = get_param(obj, "flanger_offset", p.flanger_offset, -1.0f, 1.0f, pointer);
    p.flanger_sweep = get_param(obj, "flanger_sweep", p.flanger_sweep, -1.0f, 1.0f, pointer);
    p.lpf_cutoff = get_param(obj, "lpf_cutoff", p.lpf_cutoff, 0.0f, 1.0f, pointer);
    p.lpf_sweep = get_param(obj, "lpf_sweep", p.lpf_sweep, -1.0f, 1.0f, pointer);
    p.lpf_resonance = get_param(obj, "lpf_resonance", p.lpf_resonance, 0.0f, 1.0f, pointer);
    p.hpf_cutoff = get_param(obj, "hpf_cutoff", p.hpf_cutoff, 0.0f, 1.0f, pointer);
    p.hpf_sweep = get_param(obj, "hpf_sweep", p.hpf_sweep, -1.0f, 1.0f, pointer);
    p.volume_db = get_param(obj, "volume_db", p.volume_db, -24.0f, 6.0f, pointer);
    const float prio = get_param(obj, "priority", float(p.priority), 0.0f, 9.0f, pointer);
    p.priority = uint8_t(prio);
    if (p.sustain == 0.0f && p.decay == 0.0f) {
        fail("sustain == 0 && decay == 0 (the envelope never opens)", pointer);
    }
    return p;
}

// §5.5: a wav entry decodes to 48 kHz mono PCM via ma_decoder; stereo
// downmixes 0.5*(L+R); longer than 10 s is an error.
namespace {

bool decode_wav(const std::filesystem::path& dir,
                const std::string& rel,
                std::vector<float>& out_pcm) {
    // Validate ONLY the pack-relative string from table JSON (cycle-13
    // blocker: testing the JOINED path rejected every wav whenever the
    // table dir was absolute — always, on Windows). The pack dir itself
    // is trusted and may legitimately be absolute or carry a drive
    // letter. ':' also rejects drive-relative forms (C:foo).
    const std::filesystem::path rel_path(rel);
    if (rel_path.is_absolute() || rel.find("..") != std::string::npos ||
        rel.find(':') != std::string::npos) {
        TB_LOG_ERROR("audio", "wav path '{}' escapes the pack", rel);
        return false;
    }
    const std::filesystem::path path = dir / rel_path;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 48000);
    ma_decoder dec;
#if defined(_WIN32)
    // Narrow-char paths break on non-ASCII directories (cycle-13).
    if (ma_decoder_init_file_w(path.wstring().c_str(), &cfg, &dec) != MA_SUCCESS) {
#else
    if (ma_decoder_init_file(path.string().c_str(), &cfg, &dec) != MA_SUCCESS) {
#endif
        return false;
    }
    constexpr ma_uint64 kMaxFrames = 48000ull * 10;
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) != MA_SUCCESS) {
        ma_decoder_uninit(&dec);
        TB_LOG_ERROR("audio", "wav '{}' cannot report its length", path.string());
        return false;
    }
    if (frames > kMaxFrames) {
        ma_decoder_uninit(&dec);
        TB_LOG_ERROR("audio", "wav '{}' exceeds 10 s", path.string());
        return false;
    }
    out_pcm.resize(size_t(frames));
    ma_uint64 read = 0;
    if (!out_pcm.empty()) {
        ma_decoder_read_pcm_frames(&dec, out_pcm.data(), frames, &read);
    }
    ma_decoder_uninit(&dec);
    out_pcm.resize(size_t(read));
    return read > 0;
}

} // namespace

bool load_audio_json(const std::filesystem::path& dir, TableAudio& out) {
    const std::filesystem::path file = dir / "audio.json";
    std::ifstream in(file);
    if (!in.good()) {
        return false; // optional file: built-ins cover everything (§6)
    }
    json doc;
    try {
        doc = json::parse(in, nullptr, true, true); // comments (canon §5.5)
    } catch (const json::parse_error& e) {
        fail(std::string("parse error: ") + e.what(), "");
    }
    if (!doc.is_object()) {
        fail("root must be an object", "");
    }
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() != "patches" && it.key() != "wav" && it.key() != "songs" &&
            it.key() != "map") {
            fail("unknown top-level key '" + it.key() + "'", "/" + it.key());
        }
    }

    if (doc.contains("patches")) {
        const json& ps = doc.at("patches");
        if (!ps.is_object()) {
            fail("'patches' must be an object", "/patches");
        }
        for (auto it = ps.begin(); it != ps.end(); ++it) {
            if (it.key() == "none") {
                fail("a patch may not be named \"none\" (reserved, §6.2c)", "/patches/" + it.key());
            }
            out.patches.emplace_back(it.key(), parse_patch_json(*it, "/patches/" + it.key()));
        }
    }
    if (doc.contains("wav")) {
        const json& wv = doc.at("wav");
        if (!wv.is_object()) {
            fail("'wav' must be an object", "/wav");
        }
        for (auto it = wv.begin(); it != wv.end(); ++it) {
            if (it.key() == "none") {
                fail("a wav may not be named \"none\" (reserved, §6.2c)", "/wav/" + it.key());
            }
            if (!it->is_string()) {
                fail("wav entry must be a path string", "/wav/" + it.key());
            }
            out.wav.emplace_back(it.key(), it->get<std::string>());
        }
    }
    if (doc.contains("songs")) {
        if (!doc.at("songs").is_object()) {
            fail("'songs' must be an object", "/songs");
        }
        // Tracker music lands at M14 (04-milestones.md M11 scope out);
        // the section is validated for shape and carried as a flag so
        // the loader accepts complete §6 files today.
        out.has_songs = true;
    }
    if (doc.contains("map")) {
        const json& mp = doc.at("map");
        if (!mp.is_object()) {
            fail("'map' must be an object", "/map");
        }
        for (auto it = mp.begin(); it != mp.end(); ++it) {
            if (purpose_from_key(it.key()) < 0) {
                fail("map key '" + it.key() +
                         "' is not a §7.2 purpose (script-played sounds go in rules.lua, "
                         "§6.2)",
                     "/map/" + it.key());
            }
            if (!it->is_string()) {
                fail("map value must be a patch id or \"none\"", "/map/" + it.key());
            }
            out.map.emplace(it.key(), it->get<std::string>());
        }
    }
    return true;
}

// Shared insert: override a same-named entry in place, else append —
// with the 0xFFFF id-space cap enforced in ONE place (cycle-9 review:
// the wav loop had drifted from the patches loop's guard).
void insert_into_bank(PatchBank& bank,
                      const std::string& name,
                      PatchEntry entry,
                      const std::string& pointer) {
    const auto existing = bank.mutable_names().find(name);
    if (existing != bank.mutable_names().end()) {
        bank.mutable_entries()[existing->second] = std::move(entry); // override in place
        return;
    }
    if (bank.mutable_entries().size() >= 0xFFFF) {
        fail("patch bank exceeds 65535 entries (id space)", pointer);
    }
    bank.mutable_names().emplace(name, uint16_t(bank.mutable_entries().size()));
    bank.mutable_entries().push_back(std::move(entry));
}

std::unique_ptr<PatchBank> build_bank(const TableAudio& audio,
                                      const std::filesystem::path& dir,
                                      int (&purpose_patch)[int(SoundPurpose::Count)]) {
    // Defaults from §7.2 (built-in ids follow the §7.1 listing order).
    struct DefaultMap {
        SoundPurpose purpose;
        const char* patch;
    };

    static const DefaultMap kDefaults[] = {
        {SoundPurpose::Flipper, "flipper_clack"},
        {SoundPurpose::Slingshot, "sling_thwack"},
        {SoundPurpose::PopBumper, "pop_bumper_ding"},
        {SoundPurpose::StandupTarget, "target_thud"},
        {SoundPurpose::DropTarget, "drop_target_clunk"},
        {SoundPurpose::Spinner, "spinner_tick"},
        {SoundPurpose::Rollover, "rollover_chime"},
        {SoundPurpose::RampMade, "ramp_whoosh"},
        {SoundPurpose::Magnet, "magnet_hum"},
        {SoundPurpose::Kicker, "kicker_pop"},
        {SoundPurpose::Launch, "launch_spring"},
        {SoundPurpose::Drain, "drain_womp"},
        {SoundPurpose::TiltWarning, "tilt_warning_buzz"},
        {SoundPurpose::Tilt, "tilt_alarm"},
        {SoundPurpose::BallLock, "lock_clunk"},
        {SoundPurpose::WallHit, "target_thud"},
        {SoundPurpose::BallBall, "lock_clunk"},
        {SoundPurpose::MenuMove, "menu_move"},
        {SoundPurpose::MenuSelect, "menu_select"},
    };

    auto bank = PatchBank::built_ins();

    // Table patches: override a built-in's name in place (same id),
    // otherwise append at 24+ in JSON key order (§5.5).
    for (const auto& [name, patch] : audio.patches) {
        PatchEntry e;
        e.name = name;
        e.priority = patch.priority;
        e.gain = 1.0f; // volume_db applied at render (§5.4)
        if (!render_patch(patch, name, e.pcm)) {
            fail("patch '" + name + "' rendered silence", "/patches/" + name);
        }
        insert_into_bank(*bank, name, std::move(e), "/patches/" + name);
    }
    for (const auto& [name, rel] : audio.wav) {
        PatchEntry e;
        e.name = name;
        e.priority = 5;
        e.gain = 1.0f;
        if (!decode_wav(dir, rel, e.pcm)) {
            fail("wav '" + name + "' could not be decoded from '" + rel + "'", "/wav/" + name);
        }
        insert_into_bank(*bank, name, std::move(e), "/wav/" + name);
    }

    for (const DefaultMap& d : kDefaults) {
        purpose_patch[int(d.purpose)] = bank->find(d.patch);
    }
    for (const auto& [key, target] : audio.map) {
        const int purpose = purpose_from_key(key);
        if (target == "none") {
            purpose_patch[purpose] = -1; // disabled (§6.2c)
            continue;
        }
        const int id = bank->find(target);
        if (id < 0) {
            fail("map value '" + target + "' does not resolve to a patch, wav, or built-in",
                 "/map/" + key);
        }
        purpose_patch[purpose] = id;
    }
    return bank;
}

} // namespace tb::audio
