#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tb::audio {

// 12-audio.md §5.1 — the complete sfxr parameter set. JSON keys mirror
// field names exactly; ranges and defaults are spec-owned.
enum class Wave : uint8_t { Square = 0, Saw, Sine, Noise, Triangle };

struct SfxPatch {
    Wave wave = Wave::Square;
    float duty = 0.5f;
    float duty_sweep = 0.0f;
    float attack = 0.0f;
    float sustain = 0.3f;
    float punch = 0.0f;
    float decay = 0.4f;
    float base_freq = 0.3f;
    float freq_limit = 0.0f;
    float freq_slide = 0.0f;
    float freq_delta_slide = 0.0f;
    float vib_depth = 0.0f;
    float vib_speed = 0.0f;
    float arp_mod = 0.0f;
    float arp_speed = 0.0f;
    float repeat_speed = 0.0f;
    float flanger_offset = 0.0f;
    float flanger_sweep = 0.0f;
    float lpf_cutoff = 1.0f;
    float lpf_sweep = 0.0f;
    float lpf_resonance = 0.0f;
    float hpf_cutoff = 0.0f;
    float hpf_sweep = 0.0f;
    float volume_db = 0.0f;
    uint8_t priority = 5;

    bool operator==(const SfxPatch&) const = default;
};

// Renders a patch to mono PCM at 48 kHz (12-audio.md §5): generation at
// the classic 44100 Hz with the classic constants, then a linear
// resample to 48000. Deterministic given (patch, id): the noise RNG is
// a PCG32 seeded with the FNV-1a hash of `patch_id`. The clip is
// peak-normalized to 0.891 (−1 dBFS) and then scaled by volume_db.
// Returns false when the render produced silence (all-zero — a
// tb_validate error per §5.4).
bool render_patch(const SfxPatch& patch, const std::string& patch_id, std::vector<float>& out_pcm);

// Peak amplitude for a decibel value (20 log rule); 0 dB -> 1.0.
float db_to_amp(float db);

} // namespace tb::audio
