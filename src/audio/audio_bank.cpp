#include "audio/audio_bank.h"

#include "core/log.h"

namespace tb::audio {

int PatchBank::find(const std::string& name) const {
    const auto it = by_name.find(name);
    return it == by_name.end() ? -1 : int(it->second);
}

namespace {

// §7.1 — the 24 built-ins, ids 0-23 in this exact order. Params not
// listed take the §5.1 defaults. Mirrored to assets/patches.json.
struct BuiltinDef {
    const char* name;
    SfxPatch patch;
};

const BuiltinDef kBuiltIns[24] = {
    {"flipper_clack",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.22f;
         p.sustain = 0.05f;
         p.punch = 0.35f;
         p.decay = 0.13f;
         p.lpf_cutoff = 0.42f;
         p.lpf_resonance = 0.3f;
         p.hpf_cutoff = 0.05f;
         p.volume_db = -2.0f;
         p.priority = 8;
         return p;
     }()},
    {"sling_thwack",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.30f;
         p.sustain = 0.06f;
         p.punch = 0.5f;
         p.decay = 0.18f;
         p.freq_slide = -0.25f;
         p.lpf_cutoff = 0.5f;
         p.priority = 7;
         return p;
     }()},
    {"pop_bumper_ding",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.62f;
         p.sustain = 0.08f;
         p.punch = 0.4f;
         p.decay = 0.25f;
         p.freq_slide = -0.1f;
         p.priority = 7;
         return p;
     }()},
    {"target_thud",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.15f;
         p.sustain = 0.04f;
         p.punch = 0.3f;
         p.decay = 0.12f;
         p.lpf_cutoff = 0.3f;
         p.priority = 4;
         return p;
     }()},
    {"drop_target_clunk",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.12f;
         p.sustain = 0.06f;
         p.punch = 0.45f;
         p.decay = 0.2f;
         p.freq_slide = -0.2f;
         p.lpf_cutoff = 0.35f;
         p.lpf_resonance = 0.4f;
         p.priority = 5;
         return p;
     }()},
    {"spinner_tick",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.75f;
         p.sustain = 0.02f;
         p.punch = 0.2f;
         p.decay = 0.05f;
         p.hpf_cutoff = 0.2f;
         p.volume_db = -4.0f;
         p.priority = 3;
         return p;
     }()},
    {"rollover_chime",
     [] {
         SfxPatch p;
         p.wave = Wave::Triangle;
         p.base_freq = 0.55f;
         p.sustain = 0.1f;
         p.decay = 0.35f;
         p.arp_mod = 0.35f;
         p.arp_speed = 0.6f;
         p.priority = 4;
         return p;
     }()},
    {"ramp_whoosh",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.2f;
         p.sustain = 0.25f;
         p.decay = 0.3f;
         p.freq_slide = 0.3f;
         p.lpf_cutoff = 0.6f;
         p.lpf_sweep = 0.2f;
         p.hpf_cutoff = 0.1f;
         p.volume_db = -3.0f;
         p.priority = 5;
         return p;
     }()},
    {"magnet_hum",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.duty = 0.25f;
         p.base_freq = 0.09f;
         p.sustain = 0.5f;
         p.decay = 0.2f;
         p.vib_depth = 0.3f;
         p.vib_speed = 0.4f;
         p.lpf_cutoff = 0.25f;
         p.volume_db = -6.0f;
         p.priority = 4;
         return p;
     }()},
    {"kicker_pop",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.45f;
         p.sustain = 0.04f;
         p.punch = 0.6f;
         p.decay = 0.2f;
         p.freq_slide = -0.4f;
         p.priority = 6;
         return p;
     }()},
    {"launch_spring",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.12f;
         p.sustain = 0.15f;
         p.decay = 0.25f;
         p.freq_slide = 0.55f;
         p.lpf_cutoff = 0.5f;
         p.lpf_sweep = 0.3f;
         p.priority = 6;
         return p;
     }()},
    {"drain_womp",
     [] {
         SfxPatch p;
         p.wave = Wave::Saw;
         p.base_freq = 0.28f;
         p.sustain = 0.2f;
         p.punch = 0.2f;
         p.decay = 0.45f;
         p.freq_slide = -0.3f;
         p.lpf_cutoff = 0.4f;
         p.lpf_sweep = -0.2f;
         p.priority = 8;
         return p;
     }()},
    {"tilt_warning_buzz",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.duty = 0.2f;
         p.base_freq = 0.18f;
         p.sustain = 0.3f;
         p.decay = 0.1f;
         p.vib_depth = 0.6f;
         p.vib_speed = 0.7f;
         p.priority = 8;
         return p;
     }()},
    {"tilt_alarm",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.duty = 0.3f;
         p.base_freq = 0.35f;
         p.sustain = 0.6f;
         p.decay = 0.15f;
         p.vib_depth = 1.0f;
         p.vib_speed = 0.15f;
         p.priority = 9;
         return p;
     }()},
    {"jackpot_hit",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.7f;
         p.sustain = 0.3f;
         p.punch = 0.5f;
         p.decay = 0.45f;
         p.arp_mod = 0.45f;
         p.arp_speed = 0.55f;
         p.flanger_offset = 0.25f;
         p.flanger_sweep = 0.1f;
         p.priority = 8;
         return p;
     }()},
    {"lock_clunk",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.1f;
         p.sustain = 0.08f;
         p.punch = 0.5f;
         p.decay = 0.25f;
         p.lpf_cutoff = 0.3f;
         p.lpf_resonance = 0.5f;
         p.priority = 6;
         return p;
     }()},
    {"multiball_riser",
     [] {
         SfxPatch p;
         p.wave = Wave::Saw;
         p.base_freq = 0.15f;
         p.sustain = 0.7f;
         p.decay = 0.3f;
         p.freq_slide = 0.35f;
         p.freq_delta_slide = 0.1f;
         p.vib_depth = 0.2f;
         p.vib_speed = 0.5f;
         p.flanger_offset = 0.3f;
         p.flanger_sweep = 0.15f;
         p.lpf_cutoff = 0.6f;
         p.lpf_sweep = 0.25f;
         p.priority = 9;
         return p;
     }()},
    {"extra_ball_fanfare",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.5f;
         p.sustain = 0.5f;
         p.punch = 0.3f;
         p.decay = 0.4f;
         p.arp_mod = 0.6f;
         p.arp_speed = 0.35f;
         p.repeat_speed = 0.55f;
         p.priority = 9;
         return p;
     }()},
    {"menu_move",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.55f;
         p.sustain = 0.03f;
         p.decay = 0.09f;
         p.hpf_cutoff = 0.15f;
         p.volume_db = -6.0f;
         p.priority = 2;
         return p;
     }()},
    {"menu_select",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.5f;
         p.sustain = 0.08f;
         p.punch = 0.2f;
         p.decay = 0.18f;
         p.freq_slide = 0.25f;
         p.volume_db = -4.0f;
         p.priority = 2;
         return p;
     }()},
    {"add_player",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.base_freq = 0.42f;
         p.sustain = 0.1f;
         p.punch = 0.2f;
         p.decay = 0.22f;
         p.arp_mod = 0.4f;
         p.arp_speed = 0.7f;
         p.volume_db = -3.0f;
         p.priority = 5;
         return p;
     }()},
    {"knocker",
     [] {
         SfxPatch p;
         p.wave = Wave::Noise;
         p.base_freq = 0.32f;
         p.sustain = 0.03f;
         p.punch = 0.9f;
         p.decay = 0.16f;
         p.freq_slide = -0.3f;
         p.lpf_cutoff = 0.6f;
         p.lpf_resonance = 0.3f;
         p.volume_db = 2.0f;
         p.priority = 9;
         return p;
     }()},
    {"timer_low",
     [] {
         SfxPatch p;
         p.wave = Wave::Square;
         p.duty = 0.3f;
         p.base_freq = 0.58f;
         p.sustain = 0.03f;
         p.decay = 0.08f;
         p.hpf_cutoff = 0.15f;
         p.volume_db = -4.0f;
         p.priority = 6;
         return p;
     }()},
    {"bonus_tick",
     [] {
         SfxPatch p;
         p.wave = Wave::Triangle;
         p.base_freq = 0.5f;
         p.sustain = 0.02f;
         p.decay = 0.07f;
         p.freq_slide = 0.15f;
         p.volume_db = -5.0f;
         p.priority = 4;
         return p;
     }()},
};

} // namespace

std::unique_ptr<PatchBank> PatchBank::built_ins() {
    auto bank = std::make_unique<PatchBank>();
    bank->entries.reserve(32);
    for (const BuiltinDef& def : kBuiltIns) {
        PatchEntry e;
        e.name = def.name;
        e.priority = def.patch.priority;
        e.gain = 1.0f; // volume_db is applied at render (§5.4)
        // A silent render still occupies its id slot (ids 0-23 are the
        // listed order — skipping would shift every later id); the
        // mixer no-ops empty PCM.
        if (!render_patch(def.patch, def.name, e.pcm)) {
            TB_LOG_WARN("audio", "built-in patch '{}' rendered silence", def.name);
            e.pcm.clear();
        }
        bank->by_name.emplace(e.name, uint16_t(bank->entries.size()));
        bank->entries.push_back(std::move(e));
    }
    return bank;
}

const char* purpose_key(SoundPurpose p) {
    switch (p) {
    case SoundPurpose::Flipper:
        return "flipper";
    case SoundPurpose::Slingshot:
        return "slingshot";
    case SoundPurpose::PopBumper:
        return "pop_bumper";
    case SoundPurpose::StandupTarget:
        return "standup_target";
    case SoundPurpose::DropTarget:
        return "drop_target";
    case SoundPurpose::Spinner:
        return "spinner";
    case SoundPurpose::Rollover:
        return "rollover";
    case SoundPurpose::RampMade:
        return "ramp_made";
    case SoundPurpose::Magnet:
        return "magnet";
    case SoundPurpose::Kicker:
        return "kicker";
    case SoundPurpose::Launch:
        return "launch";
    case SoundPurpose::Drain:
        return "drain";
    case SoundPurpose::TiltWarning:
        return "tilt_warning";
    case SoundPurpose::Tilt:
        return "tilt";
    case SoundPurpose::BallLock:
        return "ball_lock";
    case SoundPurpose::WallHit:
        return "wall_hit";
    case SoundPurpose::BallBall:
        return "ball_ball";
    case SoundPurpose::MenuMove:
        return "menu_move";
    case SoundPurpose::MenuSelect:
        return "menu_select";
    default:
        return "?";
    }
}

int purpose_from_key(const std::string& key) {
    for (int i = 0; i < int(SoundPurpose::Count); ++i) {
        if (key == purpose_key(SoundPurpose(uint8_t(i)))) {
            return i;
        }
    }
    return -1;
}

} // namespace tb::audio
