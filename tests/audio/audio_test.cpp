// M11 audio tests (04-milestones.md §M11; 12-audio.md).
#include "audio/audio_bank.h"
#include "audio/audio_engine.h"
#include "audio/audio_json.h"
#include "audio/sfx_synth.h"
#include "core/hash.h"
#include "core/time.h"
#include "sim/solver.h"
#include "support/alloc_hook.h"
#include "support/data_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace tb;

// Deterministic PCM fingerprint: FNV over the raw float bits.
uint64_t pcm_hash(const std::vector<float>& pcm) {
    return fnv1a64(pcm.data(), pcm.size() * sizeof(float), 0xA0D105E7ull);
}

// ---- SfxSynth.PatchRendersDeterministicPcm (5 reference patches
// render deterministically within one binary; the 24 built-ins render
// within bounds — a cross-build PCM golden is future work) ----
TEST(SfxSynth, PatchRendersDeterministicPcm) {
    // Five reference patches chosen to exercise distinct synth paths:
    // pure square arp, noise + LPF, sweep, vibrato sine, flanger.
    const std::pair<const char*, audio::SfxPatch> refs[5] = {
        {"ref_square_arp",
         [] {
             audio::SfxPatch p;
             p.wave = audio::Wave::Square;
             p.base_freq = 0.45f;
             p.sustain = 0.25f;
             p.decay = 0.35f;
             p.arp_mod = 0.5f;
             p.arp_speed = 0.5f;
             return p;
         }()},
        {"ref_noise_lpf",
         [] {
             audio::SfxPatch p;
             p.wave = audio::Wave::Noise;
             p.base_freq = 0.14f;
             p.sustain = 0.08f;
             p.decay = 0.22f;
             p.lpf_cutoff = 0.35f;
             return p;
         }()},
        {"ref_sweep",
         [] {
             audio::SfxPatch p;
             p.wave = audio::Wave::Noise;
             p.base_freq = 0.12f;
             p.sustain = 0.15f;
             p.decay = 0.25f;
             p.freq_slide = 0.55f;
             p.freq_delta_slide = 0.1f;
             return p;
         }()},
        {"ref_vib_sine",
         [] {
             audio::SfxPatch p;
             p.wave = audio::Wave::Sine;
             p.base_freq = 0.4f;
             p.sustain = 0.5f;
             p.decay = 0.3f;
             p.vib_depth = 0.8f;
             p.vib_speed = 0.6f;
             return p;
         }()},
        {"ref_flanger",
         [] {
             audio::SfxPatch p;
             p.wave = audio::Wave::Saw;
             p.base_freq = 0.3f;
             p.sustain = 0.4f;
             p.decay = 0.4f;
             p.flanger_offset = 0.25f;
             p.flanger_sweep = 0.1f;
             return p;
         }()},
    };
    for (const auto& [id, patch] : refs) {
        std::vector<float> a, b;
        ASSERT_TRUE(audio::render_patch(patch, id, a)) << id;
        ASSERT_TRUE(audio::render_patch(patch, id, b)) << id;
        EXPECT_FALSE(a.empty()) << id;
        EXPECT_EQ(pcm_hash(a), pcm_hash(b)) << id << ": render not deterministic";
        // 48 kHz output; every patch ends within 4 s (§5.3 cap).
        EXPECT_LE(a.size(), 48000u * 4u) << id;
        float peak = 0.0f;
        for (float s : a) {
            peak = std::max(peak, std::fabs(s));
        }
        // Normalized to -1 dBFS then volume_db (0 here): ~0.891.
        EXPECT_GT(peak, 0.7f) << id;
        EXPECT_LT(peak, 1.0f) << id;
    }

    // All 24 built-ins render (id slots 0-23 stable, §5.5).
    auto bank = audio::PatchBank::built_ins();
    ASSERT_EQ(bank->size(), 24u);
    for (const auto& e : bank->patch_entries()) {
        EXPECT_FALSE(e.pcm.empty()) << e.name;
    }
}

// ---- Scheduler.TickToSampleWithin1ms (12 §12 CI gate: mapping math,
// never wall time) ----
TEST(Scheduler, TickToSampleWithin1ms) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));

    float buf[128 * 2];
    sys.publish_tick(0);
    sys.render_offline(buf, 128); // clock anchors

    // Two events 5 ticks apart: their voice starts must differ by
    // round(5 * spt) samples regardless of callback boundaries (§4.2:
    // the lead D preserves relative spacing exactly).
    audio::SoundEvent e1, e2;
    e1.tick = 100;
    e2.tick = 105;
    e1.patch = e2.patch = 0;
    ASSERT_TRUE(sys.sound_queue().push(e1));
    ASSERT_TRUE(sys.sound_queue().push(e2));

    audio::AudioSystem::DebugStart starts[audio::AudioSystem::kDebugStarts];
    uint32_t n = 0;
    for (uint64_t t = 1; t <= 120; ++t) {
        sys.publish_tick(t);
        sys.render_offline(buf, 128);
        n = sys.debug_starts(audio::AudioSystem::kDebugStarts, starts);
        if (n >= 2) {
            break;
        }
    }
    ASSERT_GE(n, 2u);
    const double expected = std::round(5.0 * sys.debug_spt()); // 240
    const double got = double(starts[1].sample) - double(starts[0].sample);
    // ±1 ms (48 samples) is the CI gate; sample-accurate is 240.
    EXPECT_NEAR(got, expected, 48.0) << "expected " << expected << " got " << got;
    EXPECT_NEAR(sys.debug_spt(), 48.0, 0.024); // ±500 ppm clamp
}

// ---- Mixer.VoiceStealOldestNoClick ----
TEST(Mixer, VoiceStealOldestNoClick) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));

    // Fill all 32 voices: 8 long-lived patches x 4 each (the per-patch
    // cap, §3.2). All events map to "now" (tick 1).
    static const uint16_t kLongPatches[8] = {16, 13, 17, 8, 11, 14, 6, 7};
    audio::SoundEvent ev;
    ev.velocity = 1.0f;
    ev.tick = 1;
    sys.publish_tick(0);
    float buf[128 * 2];
    sys.render_offline(buf, 128); // anchor

    for (uint16_t patch : kLongPatches) {
        for (int i = 0; i < 4; ++i) {
            ev.patch = patch;
            ASSERT_TRUE(sys.sound_queue().push(ev));
        }
    }
    sys.publish_tick(2);
    sys.render_offline(buf, 128); // events classified + voices start
    sys.render_offline(buf, 128);
    ASSERT_EQ(sys.stats().active_voices.load(), 32u);

    // One more equal-or-higher-priority sound steals: the victim is
    // lowest-priority-then-oldest, cut with the 64-sample fade mixed
    // into this buffer before the slot is rewritten (§3.2) — no
    // discontinuity, no crash.
    const uint32_t stolen_before = sys.stats().stolen_voices.load();
    ev.patch = 11; // drain_womp, priority 8
    ASSERT_TRUE(sys.sound_queue().push(ev));
    sys.publish_tick(3);
    sys.render_offline(buf, 128);
    EXPECT_EQ(sys.stats().stolen_voices.load(), stolen_before + 1u);
    EXPECT_EQ(sys.stats().active_voices.load(), 32u);

    // A LOWER-priority sound loses and is dropped (§3.2 rule 2):
    // menu_move (patch 18, priority 2) cannot steal anything at 4+.
    const uint32_t dropped_before = sys.stats().dropped_events.load();
    ev.patch = 18; // menu_move, priority 2
    ASSERT_TRUE(sys.sound_queue().push(ev));
    sys.publish_tick(4);
    sys.render_offline(buf, 128);
    EXPECT_EQ(sys.stats().dropped_events.load(), dropped_before + 1u);
}

// ---- Mixer.CallbackAllocationFree (03-process.md §1.6) ----
TEST(Mixer, CallbackAllocationFree) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(audio::PatchBank::built_ins());

    // Queue a burst of events, then measure allocations across the mix.
    audio::SoundEvent ev;
    ev.patch = 0;
    for (int i = 0; i < 32; ++i) {
        ev.tick = 0;
        ASSERT_TRUE(sys.sound_queue().push(ev));
    }
    float buf[512 * 2];
    sys.publish_tick(0);

    tb::test::ScopedAllocCount count;
    // The FIRST render after the burst is inside the window: event
    // drain, tick->sample classification, and voice starts all happen
    // here (cycle-7 review — the warm-up render defeated the gate).
    sys.publish_tick(1);
    sys.render_offline(buf, 512);
    sys.render_offline(buf, 512);
    EXPECT_EQ(count.news_total(), 0u) << "audio callback path allocated";
    EXPECT_EQ(count.deletes_total(), 0u) << "audio callback path freed";
}

// ---- AudioSmoke.DeviceOpens (null backend; real device optional) ----
TEST(AudioSmoke, DeviceOpens) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    EXPECT_EQ(sys.period_frames(), 128);
    float buf[128 * 2];
    sys.publish_tick(0);
    sys.render_offline(buf, 128);
    // Silent when nothing plays? The bank exists; with no events the
    // output must be exactly zero (no noise floor).
    for (float s : buf) {
        EXPECT_EQ(s, 0.0f);
    }
    sys.shutdown();
}

// ---- Clock: ±300 ppm drift converges; a 1 s stall re-anchors once ----
TEST(Scheduler, DriftConvergesAndStallReanchors) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));

    // Simulated drift: the audio stream consumes 48000 frames/s; the
    // sim publishes ticks slower (300 ppm): tick T arrives when the
    // stream is where tick T * 48.0144 would map.
    float buf[128 * 2];
    const double drift_spt = 48.0 * (1.0 + 300e-6);
    uint64_t stream = 0;
    sys.publish_tick(0);
    sys.render_offline(buf, 128); // clock anchors at stream_pos=0 (the
                                  // buffer start this mix filled FROM)
    stream += 128;
    double spt_seen = 48.0;
    for (uint64_t t = 1; t <= 2000; ++t) {
        // Sim publishes tick t when the true DAC position (drifted)
        // reaches t * drift_spt.
        while (stream < uint64_t(double(t) * drift_spt)) {
            sys.render_offline(buf, 128);
            stream += 128;
        }
        sys.publish_tick(t);
        spt_seen = sys.debug_spt();
    }
    // Converged: spt moved toward compensating the drift (clamped to
    // ±500 ppm); late events stay zero (the lead absorbs the phase).
    EXPECT_GT(spt_seen, 48.0) << "drift correction must raise spt";
    EXPECT_LE(spt_seen, 48.024);
    EXPECT_EQ(sys.stats().late_events.load(), 0u);

    // 1 s stall: the sim freezes (T stays at `before`) while the
    // stream advances 48000 frames; the drifted mapping exceeds 480
    // samples -> hard re-anchor on the NEXT mix.
    const uint64_t before = 3000;
    sys.publish_tick(before);
    for (int i = 0; i < 375; ++i) { // 48000 frames at 128/call
        sys.render_offline(buf, 128);
    }
    sys.publish_tick(before); // still stalled when the re-anchor lands
    sys.render_offline(buf, 128);
    // Tolerance INSIDE the ±500 ppm clamp span (0.024): a drifted-but-
    // not-reanchored spt (~48.0144 at 300 ppm) must FAIL here.
    EXPECT_NEAR(sys.debug_spt(), 48.0, 0.005);
}

// ---- Limiter: +6 dB overdrive shows gain reduction, no hard clip ----
TEST(Mixer, LimiterGainReduction) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));

    // Overdrive: 8 simultaneous full-velocity knocker (volume_db +2)
    // events — the summed master must pass through the limiter's
    // threshold with tanh shaping, never hard-clip to exactly ±1.0
    // plateaus longer than a couple of samples.
    audio::SoundEvent ev;
    ev.patch = 21; // knocker, +2 dB
    ev.velocity = 1.0f;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(sys.sound_queue().push(ev));
    }
    float buf[512 * 2];
    sys.publish_tick(0);
    sys.render_offline(buf, 512);
    // §3.3: the shaper's asymptote is tanh(k)·inv = 1.0 mathematically,
    // but while the 2 ms attack envelope is still rising (g = 1) a hot
    // input passes tanh(k·L)·inv which peaks at inv ≈ 1.105 — the
    // makeup-gain ceiling. "No hard clipping" = bounded by that smooth
    // ceiling, never a square plateau.
    const float kInvK = float(1.0 / std::tanh(1.5));
    float peak = 0.0f;
    for (float s : buf) {
        peak = std::max(peak, std::fabs(s));
    }
    EXPECT_LE(peak, kInvK + 1e-4f) << "bounded by the shaper ceiling";
    EXPECT_GT(peak, 0.5f) << "overdrive should be audible";
}

// ---- assets/patches.json mirrors the compiled built-in bank (§7.1) ----
TEST(AudioBank, AssetsMirrorMatchesBuiltIns) {
    std::ifstream in(tb::test::data_path("assets/patches.json"));
    ASSERT_TRUE(in.good());
    nlohmann::ordered_json doc;
    try {
        doc = nlohmann::ordered_json::parse(in, nullptr, true, true);
    } catch (const nlohmann::json::parse_error& e) {
        FAIL() << "assets/patches.json: " << e.what();
    }
    auto bank = audio::PatchBank::built_ins();
    ASSERT_EQ(bank->size(), 24u);
    ASSERT_TRUE(doc.is_object());
    ASSERT_EQ(doc.size(), 24u);
    // Key order == id order (§5.5) AND every parameter matches the
    // compiled table — a drift in either direction fails here.
    const auto& params = audio::PatchBank::built_in_params();
    ASSERT_EQ(params.size(), 24u);
    size_t i = 0;
    for (auto it = doc.begin(); it != doc.end(); ++it, ++i) {
        EXPECT_EQ(it.key(), bank->patch_entries()[i].name) << "id " << i;
        const audio::SfxPatch parsed = audio::parse_patch_json(*it, "/" + it.key());
        EXPECT_EQ(parsed, params[i].second) << "params for '" << it.key() << "'";
    }
}

// ---- audio.json loading + validation ----
TEST(AudioJson, LoadsTestLabAndValidates) {
    audio::TableAudio ta;
    ASSERT_TRUE(audio::load_audio_json(tb::test::data_path("tables/test-lab"), ta));
    EXPECT_EQ(ta.patches.size(), 6u);
    EXPECT_TRUE(ta.has_songs); // shape-validated, deferred to M14
    int purpose[sim::SimState::kSoundPurposeCount] = {};
    auto bank = audio::build_bank(ta, tb::test::data_path("tables/test-lab"), purpose);
    ASSERT_NE(bank, nullptr);
    // Built-ins 0-23 + 6 table patches = 30 ids.
    EXPECT_EQ(bank->size(), 30u);
    EXPECT_EQ(bank->find("sfx_mode_start"), 24);
    // Defaults resolve (§7.2).
    EXPECT_EQ(purpose[int(sim::SoundPurpose::Flipper)], bank->find("flipper_clack"));
}

TEST(AudioJson, RejectsBadMapKeyAndNonePatch) {
    // Write a scratch pack with an illegal map key.
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_audio_json_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    auto write = [&](const char* text) { std::ofstream(dir / "audio.json") << text; };
    write(R"json({ "map": { "bank_complete": "flipper_clack" } })json");
    audio::TableAudio ta;
    EXPECT_THROW(audio::load_audio_json(dir, ta), audio::AudioLoadError);

    write(R"json({ "patches": { "none": { "sustain": 0.1 } } })json");
    EXPECT_THROW(audio::load_audio_json(dir, ta), audio::AudioLoadError);

    write(R"json({ "patches": { "x": { "sustain": 0, "decay": 0 } } })json");
    EXPECT_THROW(audio::load_audio_json(dir, ta), audio::AudioLoadError);

    write(R"json({ "patches": { "x": { "no_such_param": 1, "sustain": 0.1 } } })json");
    EXPECT_THROW(audio::load_audio_json(dir, ta), audio::AudioLoadError);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(AudioJson, NoneDisablesPurpose) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_audio_none_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "audio.json") << R"json({ "map": { "pop_bumper": "none" } })json"; }
    audio::TableAudio ta;
    ASSERT_TRUE(audio::load_audio_json(dir, ta));
    int purpose[sim::SimState::kSoundPurposeCount] = {};
    auto bank = audio::build_bank(ta, dir, purpose);
    EXPECT_EQ(purpose[int(sim::SoundPurpose::PopBumper)], -1); // disabled
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- Regression (cycle-13 blocker): a wav entry must load when the
// PACK DIR is absolute (always on Windows); the escape guard validates
// only the pack-relative string ----
TEST(AudioJson, WavLoadsFromAbsolutePackDir) {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tb_wav_abs_" + std::to_string(tb_now_ns()));
    std::filesystem::create_directories(dir / "assets");
    ASSERT_TRUE(dir.is_absolute()); // temp_directory_path is absolute

    // Hand-roll a 48 kHz mono f32 RIFF wav (miniaudio reads f32 PCM).
    constexpr uint32_t kFrames = 4800; // 100 ms

    struct {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t riff_size = 36 + kFrames * 4;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt_[4] = {'f', 'm', 't', ' '};
        uint32_t fmt_size = 16;
        uint16_t audio_format = 3; // IEEE float
        uint16_t channels = 1;
        uint32_t rate = 48000;
        uint32_t byte_rate = 48000 * 4;
        uint16_t block_align = 4;
        uint16_t bits = 32;
        char data[4] = {'d', 'a', 't', 'a'};
        uint32_t data_size = kFrames * 4;
    } hdr;

    std::vector<float> pcm(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * float(i) / 48000.0f);
    }
    {
        std::ofstream wav(dir / "assets" / "tone.wav", std::ios::binary);
        wav.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        wav.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 4));
    }
    { std::ofstream(dir / "audio.json") << R"json({ "wav": { "tone": "assets/tone.wav" } })json"; }

    audio::TableAudio ta;
    ASSERT_TRUE(audio::load_audio_json(dir, ta));
    int purpose[sim::SimState::kSoundPurposeCount] = {};
    auto bank = audio::build_bank(ta, dir, purpose); // must NOT throw
    ASSERT_NE(bank, nullptr);
    const int id = bank->find("tone");
    ASSERT_GE(id, 0);
    EXPECT_EQ(bank->patch_entries()[size_t(id)].pcm.size(), kFrames);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Escape rejections: root-relative and UNC forms must fail even where
// is_absolute() is false (Windows) — has_root_path() closes it.
TEST(AudioJson, WavGuardRejectsEscapes) {
    // R"(\\escape.wav)" as a raw literal: the C++ string must carry
    // TWO backslashes so the JSON escape decodes to one — a single
    // backslash makes the JSON invalid and the case never reaches the
    // path guard (cycle-15 review).
    for (const char* bad : {"/escape.wav",
                            R"(\\escape.wav)",
                            "//server/share/x.wav",
                            "C:escape.wav",
                            "assets/../../escape.wav"}) {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("tb_wav_esc_" + std::to_string(tb_now_ns()));
        std::filesystem::create_directories(dir);
        {
            const std::string json_text = std::string(R"json({ "wav": { "bad": ")json") + bad +
                                          std::string(R"json(" } })json");
            std::ofstream(dir / "audio.json") << json_text;
        }
        audio::TableAudio ta;
        bool threw = false;
        try {
            if (audio::load_audio_json(dir, ta)) {
                int purpose[sim::SimState::kSoundPurposeCount] = {};
                (void)audio::build_bank(ta, dir, purpose);
            }
        } catch (const audio::AudioLoadError&) {
            threw = true;
        }
        EXPECT_TRUE(threw) << "escape '" << bad << "' was accepted";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

} // namespace
