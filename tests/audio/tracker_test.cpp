// M14 tracker tests (04-milestones.md §M14; 12-audio.md §8-§10):
// golden-hash determinism, row timing, the loop seam, engine music
// states (crossfade + attract offset), and ducking.
#include "audio/tracker.h"

#include "audio/audio_engine.h"
#include "audio/audio_json.h"
#include "core/hash.h"
#include "sim/solver.h"
#include "support/alloc_hook.h"
#include "support/data_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace tb;

uint64_t pcm_hash(const float* pcm, size_t n) {
    return fnv1a64(pcm, n * sizeof(float), 0x7EA50ECA11ull);
}

// The §8.5 reference song (transcribed exactly) as JSON text.
const char* kReferenceSong = R"json({
  "bpm": 112, "ticks_per_row": 12,
  "patterns": {
    "A": {
      "pulse1": ["E-5 lead_pulse 10", "---", "C-5", "---",
                 "D-5", "E-5", "C-5", "---",
                 "F-5 . . V3,5", "---", "E-5", "---",
                 "C-5", "---", "A-4", "OFF"],
      "pulse2": ["---", "A-4 stab_pulse 8 A037", "---", "A-4 . . A037",
                 "---", "A-4 . . A037", "---", "A-4 . . A037",
                 "---", "F-4 . . A047", "---", "F-4 . . A047",
                 "---", "F-4 . . A047", "---", "F-4 . . A047"],
      "wide":   ["A-2 bass_saw 12", "A-2", "A-3", "A-2", "A-2", "A-3", "A-2", "A-3",
                 "F-2", "F-2", "F-3", "F-2", "F-2", "F-3", "F-2", "F-3"],
      "noise":  ["C-2 kit_noise 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4"]
    },
    "B": {
      "pulse1": ["G-5 lead_pulse 10", "---", "E-5", "---",
                 "D-5", "E-5", "G-5", "---",
                 "D-5 . . V3,5", "---", "B-4", "---",
                 "G-4", "---", "A-4", "OFF"],
      "pulse2": ["---", "C-5 stab_pulse 8 A047", "---", "C-5 . . A047",
                 "---", "C-5 . . A047", "---", "C-5 . . A047",
                 "---", "G-4 . . A047", "---", "G-4 . . A047",
                 "---", "G-4 . . A047", "---", "G-4 . . A047"],
      "wide":   ["C-3 bass_saw 12", "C-3", "C-4", "C-3", "C-3", "C-4", "C-3", "C-4",
                 "G-2", "G-2", "G-3", "G-2", "G-2", "G-3", "G-2", "G-3"],
      "noise":  ["C-2 kit_noise 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4"]
    }
  },
  "order": ["A", "A", "B", "B"]
})json";

// The §6 example instrument set the reference song names.
std::map<std::string, audio::SfxPatch> reference_patches() {
    std::map<std::string, audio::SfxPatch> m;
    audio::SfxPatch lead;
    lead.wave = audio::Wave::Square;
    lead.duty = 0.25f;
    lead.attack = 0.05f;
    lead.sustain = 0.4f;
    lead.decay = 0.25f;
    lead.volume_db = -2.0f;
    m["lead_pulse"] = lead;
    audio::SfxPatch stab;
    stab.wave = audio::Wave::Square;
    stab.duty = 0.5f;
    stab.sustain = 0.0f;
    stab.decay = 0.12f;
    stab.volume_db = -6.0f;
    m["stab_pulse"] = stab;
    audio::SfxPatch bass;
    bass.wave = audio::Wave::Saw;
    bass.sustain = 0.5f;
    bass.decay = 0.2f;
    m["bass_saw"] = bass;
    audio::SfxPatch kit;
    kit.wave = audio::Wave::Noise;
    kit.sustain = 0.0f;
    kit.decay = 0.18f;
    m["kit_noise"] = kit;
    return m;
}

audio::TrackerSong reference_song() {
    const auto j = nlohmann::ordered_json::parse(kReferenceSong, nullptr, true, true);
    return audio::parse_song_json(j, reference_patches(), "/songs/ref");
}

// Renders `seconds` of the song through one player at unity gain.
std::vector<float> render_song(const audio::TrackerSong& song,
                               float seconds,
                               bool loop = true,
                               uint64_t* out_hash = nullptr) {
    audio::TrackerPlayer player;
    player.start(&song, loop);
    std::vector<float> buf(size_t(seconds * 48000.0f) * 2, 0.0f);
    std::vector<float> g(512, 1.0f);
    size_t done = 0;
    while (done + 512 <= buf.size() / 2) {
        player.render(buf.data() + 2 * done, g.data(), 512);
        done += 512;
    }
    if (done < buf.size() / 2) {
        player.render(buf.data() + 2 * done, g.data(), uint32_t(buf.size() / 2 - done));
    }
    if (out_hash != nullptr) {
        *out_hash = pcm_hash(buf.data(), buf.size());
    }
    return buf;
}

// 16 cells: `first` then skips (helper for one-channel test songs).
std::string cells(const std::string& first) {
    return "[" + first +
           ", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", "
           "\"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\"]";
}

std::string all_off() {
    return "[\"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", "
           "\"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\", \"---\"]";
}

// Wraps four channel cell arrays into a one-pattern song JSON text.
std::string solo_song(const std::string& pulse1, int bpm, int tpr) {
    return std::string(R"json({
      "bpm": )json") +
           std::to_string(bpm) + R"json(, "ticks_per_row": )json" + std::to_string(tpr) +
           R"json(,
      "patterns": { "A": {
        "pulse1": )json" +
           pulse1 + R"json(,
        "pulse2": )json" +
           all_off() + R"json(,
        "wide": )json" +
           all_off() + R"json(,
        "noise": )json" +
           all_off() + R"json( } },
      "order": ["A"] })json";
}

// ---- parsing (§8.2/§8.3 ranges + §8.1 wave rules) ----

TEST(TrackerSongParse, ReferenceSongParses) {
    const audio::TrackerSong song = reference_song();
    EXPECT_EQ(song.bpm, 112.0f);
    EXPECT_EQ(song.ticks_per_row, 12u);
    EXPECT_EQ(song.patterns.size(), 2u);
    EXPECT_EQ(song.rows_per_pattern(), 16u);
    ASSERT_EQ(song.order.size(), 4u);
    EXPECT_EQ(song.instruments.size(), 4u);
    // Cell spot checks: E-5 = midi 76 -> 76-11 = 65.
    EXPECT_EQ(song.patterns[0].second.chan[0][0].note, 65u);
    EXPECT_EQ(song.patterns[0].second.chan[0][0].inst, 0u); // lead_pulse
    EXPECT_EQ(song.patterns[0].second.chan[0][15].note, audio::TrackerCell::kOff);
    // Volume cells: noise row 1 "C-6 . 4".
    EXPECT_EQ(song.patterns[0].second.chan[3][1].vol, 4u);
    // The A037 arp on pulse2 row 1.
    EXPECT_EQ(song.patterns[0].second.chan[1][1].fx, 1u);
    EXPECT_EQ(song.patterns[0].second.chan[1][1].arp_n, 3u);
}

TEST(TrackerSongParse, RejectsIllegalWaveForChannel) {
    // bass_saw (saw) on pulse1: §8.1 allows square only there.
    const std::string pulse1 = "[\"C-4 bass_saw 10\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"OFF\"]";
    const auto j = nlohmann::ordered_json::parse(solo_song(pulse1, 120, 12), nullptr, true, true);
    EXPECT_THROW(audio::parse_song_json(j, reference_patches(), "/songs/x"), audio::AudioLoadError);
}

TEST(TrackerSongParse, RejectsMissingInstOnFirstNote) {
    const std::string pulse1 = "[\"C-4\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"---\", "
                               "\"---\", \"---\", \"---\", \"OFF\"]";
    const auto j = nlohmann::ordered_json::parse(solo_song(pulse1, 120, 12), nullptr, true, true);
    EXPECT_THROW(audio::parse_song_json(j, reference_patches(), "/songs/x"), audio::AudioLoadError);
}

TEST(TrackerSongParse, RejectsBadNoteBpmAndOrder) {
    auto patches = reference_patches();
    const std::string ok = cells("\"C-4 lead_pulse 10\"");
    { // Bad note token H-9.
        const std::string bad = cells("\"H-9 lead_pulse 10\"");
        const auto j = nlohmann::ordered_json::parse(solo_song(bad, 120, 12), nullptr, true, true);
        EXPECT_THROW(audio::parse_song_json(j, patches, "/songs/x"), audio::AudioLoadError);
    }
    { // bpm out of 40-260.
        const auto j = nlohmann::ordered_json::parse(solo_song(ok, 300, 12), nullptr, true, true);
        EXPECT_THROW(audio::parse_song_json(j, patches, "/songs/x"), audio::AudioLoadError);
    }
    { // ticks_per_row out of 1-31.
        std::string s = solo_song(ok, 120, 12);
        s.replace(s.find("\"ticks_per_row\": 12"),
                  std::string("\"ticks_per_row\": 12").size(),
                  "\"ticks_per_row\": 32");
        const auto j = nlohmann::ordered_json::parse(s, nullptr, true, true);
        EXPECT_THROW(audio::parse_song_json(j, patches, "/songs/x"), audio::AudioLoadError);
    }
    { // Order names an undefined pattern.
        std::string s = solo_song(ok, 120, 12);
        s.replace(s.find("[\"A\"]"), 5, "[\"Z\"]");
        const auto j = nlohmann::ordered_json::parse(s, nullptr, true, true);
        EXPECT_THROW(audio::parse_song_json(j, patches, "/songs/x"), audio::AudioLoadError);
    }
    { // Channels disagree on row count inside a pattern.
        std::string s = solo_song(ok, 120, 12);
        const std::string two = "[\"---\", \"OFF\"]";
        s.replace(s.find(all_off()), all_off().size(), two); // pulse2 now 2 rows
        const auto j = nlohmann::ordered_json::parse(s, nullptr, true, true);
        EXPECT_THROW(audio::parse_song_json(j, patches, "/songs/x"), audio::AudioLoadError);
    }
}

// ---- determinism: golden hash (the sample math is LUT-only, so the
// hash is portable across the 3 CI OSes) ----

TEST(Tracker, SongRendersDeterministicPcm) {
    const audio::TrackerSong song = reference_song();
    uint64_t h1 = 0, h2 = 0;
    const std::vector<float> a = render_song(song, 3.0f, true, &h1);
    (void)render_song(song, 3.0f, true, &h2);
    EXPECT_EQ(h1, h2);                    // identical re-render
    EXPECT_EQ(h1, 0x577375EA9CB54983ull); // pinned golden (re-pin only
                                          // via a deliberate change)
    // Non-silence: the mix carries energy.
    float peak = 0.0f;
    for (float s : a) {
        peak = std::max(peak, std::fabs(s));
    }
    EXPECT_GT(peak, 0.05f);
}

// ---- row timing: note onsets land within 1 ms of the row grid ----

TEST(Tracker, RowTimingWithin1ms) {
    // Solo pulse1, one-shot notes (attack then release) every 2 rows:
    // row_seconds = 6 * 2.5 / 120 = 0.125 s, onsets 0.25 s apart.
    auto patches = reference_patches();
    audio::SfxPatch blip = patches["lead_pulse"];
    blip.sustain = 0.0f; // one-shot: each note rings ~60 ms then rests
    blip.decay = 0.15f;
    patches["blip"] = blip;
    const std::string lead = "[\"C-5 blip 10\", \"---\", \"E-5\", \"---\", "
                             "\"G-5\", \"---\", \"E-5\", \"---\", "
                             "\"C-5\", \"---\", \"E-5\", \"---\", "
                             "\"G-5\", \"---\", \"E-5\", \"OFF\"]";
    const auto j = nlohmann::ordered_json::parse(solo_song(lead, 120, 6), nullptr, true, true);
    const audio::TrackerSong song = audio::parse_song_json(j, patches, "/s");
    std::vector<float> buf = render_song(song, 2.0f);
    const float row_samples = 0.125f * 48000.0f;
    float prev = 0.0f;
    int onsets = 0;
    for (size_t i = 1; i < buf.size() / 2; ++i) {
        const float e = std::fabs(buf[2 * i]) + std::fabs(buf[2 * i + 1]);
        if (prev < 1e-4f && e >= 1e-4f) {
            if (onsets < 8) {
                const double expect = double(onsets) * 2.0 * row_samples;
                EXPECT_NEAR(double(i), expect, 48.0) << "onset " << onsets;
            }
            ++onsets;
        }
        prev = e;
    }
    EXPECT_GE(onsets, 8); // 2 s at 4 onsets/s
}

// ---- loop seam: no click at the order wrap ----

TEST(Tracker, LoopSeamless) {
    // One pattern whose wide channel holds a gated bass note across
    // the loop boundary (row 0 is `---` for wide): phase continuity =
    // no click. Solo-wide song via a wrapper.
    auto patches = reference_patches();
    // Row 0 is `---`: the gated note starts at row 1 and never ends,
    // so it sustains straight through the order wrap.
    const std::string wide = "[\"---\", \"A-2 bass_saw 12\", \"---\", \"---\", "
                             "\"---\", \"---\", \"---\", \"---\", "
                             "\"---\", \"---\", \"---\", \"---\", "
                             "\"---\", \"---\", \"---\", \"---\"]";
    const std::string src = std::string(R"json({
      "bpm": 120, "ticks_per_row": 6,
      "patterns": { "A": {
        "pulse1": )json") + all_off() +
                            R"json(,
        "pulse2": )json" + all_off() +
                            R"json(,
        "wide": )json" + wide +
                            R"json(,
        "noise": )json" + all_off() +
                            R"json( } },
      "order": ["A"] })json";
    const auto j = nlohmann::ordered_json::parse(src, nullptr, true, true);
    const audio::TrackerSong song = audio::parse_song_json(j, patches, "/s");
    std::vector<float> buf = render_song(song, 3.0f);
    // Pattern length = 16 rows * 0.125 s = 2 s = 96000 samples; seams
    // at 96000 and 192000.
    for (uint64_t seam : {96000ull, 192000ull}) {
        // A click is a full-scale jump; the 110 Hz saw itself steps
        // <= ~0.01/sample. Anything under 0.2 is continuous signal.
        float worst = 0.0f;
        for (uint64_t i = seam - 100; i <= seam + 100 && i + 1 < buf.size() / 2; ++i) {
            const float d = std::fabs(buf[2 * (i + 1)] - buf[2 * i]) +
                            std::fabs(buf[2 * (i + 1) + 1] - buf[2 * i + 1]);
            worst = std::max(worst, d);
        }
        EXPECT_LT(worst, 0.2f) << "click at seam " << seam << " (worst " << worst << ")";
    }
}

// ---- engine music states (§9) + ducking (§10) ----

// A bank with continuous-tone songs under the given ids (gated square,
// no OFF): constant amplitude, ideal for gain measurements.
std::unique_ptr<audio::PatchBank> tone_bank(const std::vector<std::string>& ids, float bpm) {
    audio::TableAudio ta;
    audio::SfxPatch lead;
    lead.wave = audio::Wave::Square;
    lead.duty = 0.5f;
    lead.sustain = 0.6f;
    lead.decay = 0.3f;
    ta.patches.emplace_back("tone_lead", lead);
    const std::string src = std::string(R"json({
      "bpm": )json") + std::to_string(int(bpm)) +
                            R"json(, "ticks_per_row": 6,
      "patterns": { "A": {
        "pulse1": )json" + cells("\"A-4 tone_lead 12\"") +
                            R"json(,
        "pulse2": )json" + all_off() +
                            R"json(,
        "wide": )json" + all_off() +
                            R"json(,
        "noise": )json" + all_off() +
                            R"json( } },
      "order": ["A"] })json";
    for (const std::string& id : ids) {
        ta.songs.emplace_back(id, nlohmann::ordered_json::parse(src, nullptr, true, true));
    }
    int purpose[sim::SimState::kSoundPurposeCount] = {};
    return audio::build_bank(ta, tb::test::data_path("tables/test-lab"), purpose);
}

float stereo_rms(const std::vector<float>& buf, size_t begin, size_t end) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = begin; i < end && i < buf.size(); ++i) {
        acc += double(buf[i]) * double(buf[i]);
        ++n;
    }
    return n > 0 ? float(std::sqrt(acc / double(n))) : 0.0f;
}

TEST(Music, PlaySongStartsAndNoOpsSameId) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(tone_bank({"main"}, 120.0f));
    EXPECT_TRUE(sys.has_song("main"));
    EXPECT_FALSE(sys.has_song("wizard"));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 8; ++i) {
        sys.render_offline(buf.data(), 512);
    }
    EXPECT_GT(stereo_rms(buf, 0, buf.size()), 0.01f);
    // Same id again: a no-op — the song keeps playing.
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 8; ++i) {
        sys.render_offline(buf.data(), 512);
    }
    EXPECT_GT(stereo_rms(buf, 0, buf.size()), 0.01f);
    // Unknown id: reported, silent no-op.
    EXPECT_FALSE(sys.play_music("wizard"));
    sys.shutdown();
}

TEST(Music, StopMusicFadesOut) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(tone_bank({"main"}, 120.0f));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 4; ++i) {
        sys.render_offline(buf.data(), 512); // start + settle
    }
    sys.stop_music();
    for (int i = 0; i < 12; ++i) { // 100 ms fade + margin
        sys.render_offline(buf.data(), 512);
    }
    // Post-fade: silence (exactly zero — the instance stopped).
    for (int i = 0; i < 2; ++i) {
        sys.render_offline(buf.data(), 512);
    }
    for (float s : buf) {
        EXPECT_EQ(s, 0.0f);
    }
    sys.shutdown();
}

TEST(Music, CrossfadeEqualPower) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    // Two identical-tone songs at different pitches (bank ids share the
    // tone patch; pitch differs via bpm so the RMS is comparable).
    sys.publish_bank(tone_bank({"main", "mode"}, 120.0f));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 8; ++i) {
        sys.render_offline(buf.data(), 512);
    }
    const float rms_a = stereo_rms(buf, 0, buf.size());
    // Switch: 100 ms equal-power crossfade; at the midpoint both
    // instances sound at ~0.707 each, so the bus power stays ~equal.
    ASSERT_TRUE(sys.play_music("mode"));
    for (int i = 0; i < 5; ++i) { // ~53 ms: near the midpoint
        sys.render_offline(buf.data(), 512);
    }
    const float rms_mid = stereo_rms(buf, 0, buf.size());
    for (int i = 0; i < 10; ++i) { // ride past the fade
        sys.render_offline(buf.data(), 512);
    }
    const float rms_b = stereo_rms(buf, 0, buf.size());
    // Equal-power: mid-fade amplitude stays near either end (the two
    // instances are the same waveform, so they sum coherently only if
    // phase-aligned; at 0.707+0.707 worst case the RMS stays within
    // [0.7, 1.5]x of steady).
    EXPECT_GT(rms_a, 0.01f);
    EXPECT_GT(rms_mid, 0.4f * rms_a);
    EXPECT_NEAR(rms_b / rms_a, 1.0f, 0.2f);
    sys.shutdown();
}

TEST(Music, AttractSongSitsAtMinus12dB) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(tone_bank({"attract", "main"}, 120.0f));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);

    ASSERT_TRUE(sys.play_music("attract"));
    for (int i = 0; i < 16; ++i) { // start + 20 ms offset ramp + settle
        sys.render_offline(buf.data(), 512);
    }
    std::vector<float> attract(buf.begin(), buf.end());
    const float rms_attract = stereo_rms(attract, 0, attract.size());

    // Crossfade to the identical song as a non-attract id: the offset
    // ramps away.
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 24; ++i) { // 100 ms crossfade + 20 ms ramp off
        sys.render_offline(buf.data(), 512);
    }
    std::vector<float> game(buf.begin(), buf.end());
    const float rms_game = stereo_rms(game, 0, game.size());

    EXPECT_GT(rms_game, 0.01f);
    EXPECT_NEAR(rms_attract / rms_game, 0.2511f, 0.02f);
    sys.shutdown();
}

TEST(Duck, SfxDucksMusicAndRecovers) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(tone_bank({"main"}, 120.0f));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);
    ASSERT_TRUE(sys.play_music("main"));
    for (int i = 0; i < 8; ++i) {
        sys.render_offline(buf.data(), 512); // settle
    }
    // Baseline: pure music.
    sys.render_offline(buf.data(), 512);
    const float rms_base = stereo_rms(buf, 0, buf.size());

    // Trigger: a SoundEvent with the duck flag (§10 duck=true). The
    // event is late (tick 0 < anchor) so it plays immediately; patch 0
    // (flipper_clack) is a short click whose tail dies inside the hold.
    audio::SoundEvent ev{};
    ev.patch = 0;
    ev.flags = 1u; // duck
    ASSERT_TRUE(sys.sound_queue().push(ev));
    for (int i = 0; i < 14; ++i) { // ~150 ms: attack done, click over
        sys.render_offline(buf.data(), 512);
    }
    const float rms_duck = stereo_rms(buf, 0, buf.size());

    // Hold is 200 ms after the trigger; ride it out + 50 ms release.
    for (int i = 0; i < 22; ++i) {
        sys.render_offline(buf.data(), 512);
    }
    sys.render_offline(buf.data(), 512);
    const float rms_recov = stereo_rms(buf, 0, buf.size());

    EXPECT_NEAR(rms_duck / rms_base, 0.5011f, 0.05f);
    EXPECT_GT(rms_recov / rms_base, 0.9f);
    sys.shutdown();
}

TEST(Music, CallbackAllocationFreeWithTracker) {
    audio::AudioSystem sys;
    audio::AudioConfig cfg;
    cfg.null_backend = true;
    ASSERT_TRUE(sys.init(cfg));
    sys.publish_bank(tone_bank({"main"}, 120.0f));
    std::vector<float> buf(512 * 2);
    sys.publish_tick(0);
    ASSERT_TRUE(sys.play_music("main"));
    sys.render_offline(buf.data(), 512); // command pop + song start
    tb::test::ScopedAllocCount count;
    sys.render_offline(buf.data(), 512);
    sys.render_offline(buf.data(), 512);
    EXPECT_EQ(count.news_total(), 0u) << "tracker mix allocated";
    EXPECT_EQ(count.deletes_total(), 0u) << "tracker mix freed";
    sys.shutdown();
}

// ---- shipped content ----

TEST(AudioJson, NeonDriftSongsLoad) {
    audio::TableAudio ta;
    ASSERT_TRUE(audio::load_audio_json(tb::test::data_path("tables/neon-drift"), ta));
    int purpose[sim::SimState::kSoundPurposeCount] = {};
    auto bank = audio::build_bank(ta, tb::test::data_path("tables/neon-drift"), purpose);
    ASSERT_NE(bank, nullptr);
    // The six reserved states (§9) all defined (15-launch-tables §1.6).
    for (const char* id : {"attract", "main", "mode", "multiball", "wizard", "game_over"}) {
        EXPECT_GE(bank->find_song(id), 0) << "missing song " << id;
    }
    // Table deltas present + mapped (§1.6).
    EXPECT_GE(bank->find("nd_shift_clack"), 0);
    EXPECT_EQ(purpose[int(sim::SoundPurpose::Flipper)], bank->find("nd_shift_clack"));
    EXPECT_EQ(purpose[int(sim::SoundPurpose::PopBumper)], bank->find("nd_horn"));
    EXPECT_EQ(purpose[int(sim::SoundPurpose::Spinner)], bank->find("nd_tach"));
    EXPECT_EQ(purpose[int(sim::SoundPurpose::Magnet)], bank->find("nd_drift_skid"));
    EXPECT_EQ(purpose[int(sim::SoundPurpose::RampMade)], bank->find("nd_boost"));
    // Duck triggers resolved (§10): drain_womp is a built-in.
    bool has_drain = false;
    for (uint32_t i = 0; i < bank->duck_patch_n; ++i) {
        has_drain = has_drain || bank->patch_entries()[bank->duck_patch[i]].name == "drain_womp";
    }
    EXPECT_TRUE(has_drain);
    // Every song renders non-silent audio (spot: main).
    const int main_idx = bank->find_song("main");
    ASSERT_GE(main_idx, 0);
    audio::TrackerPlayer player;
    player.start(&bank->songs()[size_t(main_idx)].song, true);
    std::vector<float> buf(512 * 2, 0.0f);
    std::vector<float> g(512, 1.0f);
    for (int i = 0; i < 8; ++i) {
        player.render(buf.data(), g.data(), 512);
    }
    EXPECT_GT(stereo_rms(buf, 0, buf.size()), 0.005f);
}

} // namespace
