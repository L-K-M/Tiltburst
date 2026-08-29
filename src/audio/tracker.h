#pragma once

#include "audio/sfx_synth.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tb::audio {

// The tracker's fixed output rate (§8.1: oscillators run at 48 kHz).
inline constexpr uint32_t kTrackerRate = 48000;

// 12-audio.md §8 — tracker music. Songs are authored as text in
// audio.json and synthesized live on the audio thread: 4 monophonic
// channels (pulse1/pulse2/wide/noise), sample-accurate rows via the
// float tick accumulator, and chip-flavored naive oscillators.
//
// Sample math is deliberately libm-free (literal LUTs for 2^(k/12),
// sin, and root-finding by Newton iteration): the rendered PCM is
// bit-identical across platforms, which the golden-hash determinism
// test relies on (16-testing-ci.md runs 3 OSes).
//
// Song PARSING lives in audio_json.h (it shares AudioLoadError and the
// patch map); this header is the data + the audio-thread player.

// The six patch fields tracker mode uses (§8.1), resolved once at bank
// build so the player never touches the patch bank at render time.
// attack/release are already §5.1-mapped seconds (x² × 2.268 s).
struct TrackerInstr {
    Wave wave = Wave::Square;
    float duty = 0.5f;
    float attack = 0.0f;
    float sustain = 0.3f; // > 0 gated, == 0 one-shot (§8.1)
    float release = 0.4f; // the decay field through the same mapping
    float gain = 1.0f;    // db_to_amp(volume_db)
};

// Parsed pattern cell (§8.3): `note [inst] [vol] [fx]`, `.` skips a
// middle token.
struct TrackerCell {
    // 0 = nothing new this row (empty or `---`); 1..108 encodes midi
    // 12..119 as midi-11 (C-0..B-8, §8.2); kOff = release.
    static constexpr uint8_t kOff = 109;
    uint8_t note = 0;
    uint8_t inst = 0xFF;                  // 0xFF = keep sticky; else instrument index
    uint8_t vol = 0xFF;                   // 0xFF = keep sticky; else 0..15
    uint8_t fx = 0;                       // 0 none, 1 arp, 2 slide, 3 vibrato
    int8_t slide = 0;                     // S±n: semitones ±1..12
    uint8_t vib_depth = 0, vib_speed = 0; // V<d>,<s>
    static constexpr size_t kArpCap = 8;
    uint8_t arp_n = 0;                     // A<digits>: digit count (<= kArpCap)
    std::array<uint8_t, kArpCap> arp = {}; // hex semitone offsets
};

struct TrackerPattern {
    static constexpr uint32_t kChannels = 4; // pulse1 pulse2 wide noise
    uint32_t rows = 16;                      // 16 or 32, song-wide
    std::array<std::vector<TrackerCell>, kChannels> chan{};
};

struct TrackerSong {
    float bpm = 112.0f;          // 40..260
    uint32_t ticks_per_row = 12; // 1..31
    std::vector<TrackerInstr> instruments;
    // Named patterns; order indexes them by name.
    std::vector<std::pair<std::string, TrackerPattern>> patterns;
    std::vector<std::string> order; // 1..128 names

    // §8.4: samples_per_tick = rate * 2.5 / bpm.
    double samples_per_tick() const { return double(kTrackerRate) * 2.5 / double(bpm); }

    uint32_t rows_per_pattern() const {
        return patterns.empty() ? 16 : patterns.front().second.rows;
    }
};

// The audio-thread player. One instance renders one song; the engine
// runs two for §9 crossfades. No allocation, locks, or libm calls
// between start() and going inactive.
class TrackerPlayer {
public:
    static constexpr uint32_t kChannels = TrackerPattern::kChannels;
    static constexpr uint32_t kFadeSamples = 64; // §8.1 retrigger fade

    // Points the player at `song` and resets to order[0], row 0. The
    // noise RNG seeds at 0x5EEDCAFE per instance per start (§8.1).
    void start(const TrackerSong* song, bool loop);
    void stop();

    bool active() const { return song_ != nullptr; }

    const TrackerSong* song() const { return song_; }

    // Renders `frames` stereo-interleaved samples, each scaled by
    // gains[i] (the crossfade curve) and ADDED into dst (the music
    // bus accumulator).
    void render(float* dst, const float* gains, uint32_t frames);

private:
    struct Channel {
        const TrackerInstr* instr = nullptr;
        float vol_gain = (12.0f / 15.0f) * (12.0f / 15.0f); // (vol/15)^2
        bool sounding = false;
        float f = 0.0f;      // current (slid) note frequency
        float f_used = 0.0f; // per-tick effective frequency (vib/arp)
        double phase = 0.0;  // [0,1) accumulator
        // Linear envelope: attack -> (gated: hold; one-shot: straight
        // into release) -> release from wherever it stood -> off.
        uint32_t attack_pos = 0, attack_len = 0;
        uint32_t release_pos = 0, release_len = 0;
        float release_start = 1.0f;
        bool releasing = false;
        bool env_off = false;
        // Vibrato (persists until note end or replacement, §8.3).
        bool vib_active = false;
        float vib_depth = 0.0f, vib_speed = 0.0f, vib_phase = 0.0f;
        // Row-scope fx state (cleaved at each row boundary).
        bool slide_this_row = false;
        float slide_per_tick = 1.0f;
        bool arp_this_row = false;
        uint8_t arp_n = 0;
        std::array<uint8_t, TrackerCell::kArpCap> arp = {};
        // Retrigger fade of the previous signal (§8.1).
        uint32_t fade_pos = kFadeSamples; // == kFadeSamples: done
        double old_phase = 0.0;
        float old_f_used = 0.0f, old_gain = 1.0f;
        float old_vol_gain = 1.0f; // the channel vol the tail was sounding at
        const TrackerInstr* old_instr = nullptr;
        // Sample-and-hold noise (noise channel only).
        double noise_acc = 0.0;
        float noise_val = 0.0f;
        // Constant-power pan (§8.4): pulse1 -0.2, pulse2 +0.2,
        // wide 0.0, noise +0.1.
        float gl = 1.0f, gr = 0.0f;
    };

    void process_row(uint32_t row);
    void apply_tick(); // fx application + per-tick f_used
    float channel_sample(Channel& ch);
    static float osc(const TrackerInstr& instr, double phase, float noise_val);

    const TrackerSong* song_ = nullptr;
    bool loop_ = true;
    bool finishing_ = false; // non-loop tail: releasing the last notes
    uint32_t order_idx_ = 0, row_idx_ = 0, tick_in_row_ = 0;
    double accum_ = 0.0; // fractional samples until the next tick
    uint64_t rng_ = 0;   // PCG32 state (0x5EEDCAFE at start, §8.1)
    std::array<Channel, kChannels> chan_{};

    uint32_t rng_next();
};

// sin(pi/2 * x): the equal-power fade curve (§9 crossfades) — shared
// with the engine's fade walk so the whole music path stays
// LUT-portable.
float tracker_eq_pow(float x01);

} // namespace tb::audio
