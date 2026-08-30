#include "audio/tracker.h"

#include "audio/audio_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace tb::audio {

namespace {

// ---- portable math (golden-hash determinism across OSes) ----

// 2^(k/12) for k = -69..50 (120 entries): f = 440 * kPow2[midi - 69]
// (§8.2) and integer-semitone ratios for the arp fx.
constexpr int kPow2N = 120;
constexpr double kPow2[kPow2N] = {
#include "tracker_pow2.inc"
};

// sin(2*pi*i/256); index 256 == index 0 (branch-free lerp).
constexpr int kSinLutN = 256;
constexpr float kSinLut[kSinLutN + 1] = {
#include "tracker_sin.inc"
};

float lut_sin(float a) {                                    // a >= 0, any magnitude (wraps)
    float x = a * (float(kSinLutN) * 0.15915494309189535f); // 1/(2pi)
    int i = int(x) % kSinLutN;
    if (i < 0) {
        i += kSinLutN;
    }
    const float f = x - std::floor(x);
    return kSinLut[size_t(i)] * (1.0f - f) + kSinLut[size_t(i + 1)] * f;
}

float lut_cos(float a) {
    return lut_sin(a + 1.5707963267948966f);
}

constexpr double kTwoPi = 6.28318530717958647692;
constexpr float kSampleRate = float(kTrackerRate);

// midi 12..119 -> frequency (§8.2). Parse guarantees the range; the
// clamp keeps a stray note (player is a public seam) in the table.
float midi_to_freq(int midi) {
    const int k = std::clamp(midi - 69, -69, 50);
    return 440.0f * float(kPow2[size_t(k + 69)]);
}

// 2^(n/12) for integer n (arp offsets).
float semitone_ratio(int n) {
    n = std::clamp(n, -69, 50);
    return float(kPow2[size_t(n + 69)]);
}

// 2^(x/12) for fractional |x| <= 12 via the 4-term Taylor of e^y
// (y = x*ln2/12; |y| <= 0.69 -> worst error ~2e-3 at the extremes,
// and the vibrato range that actually uses this is |x| <= 1.875,
// error < 1e-6). Pure arithmetic: bit-stable everywhere.
float semitone_ratio_frac(float x) {
    const float y = x * 0.05776226504666215f; // ln2/12
    return 1.0f + y * (1.0f + y * (0.5f + y * (0.16666666666666666f)));
}

// The tpr-th root of r by Newton iteration on x^tpr = r (deterministic
// double arithmetic; used for the per-tick slide factor, §8.3 S±n).
double nth_root(double r, uint32_t tpr) {
    if (r <= 0.0 || tpr <= 1) {
        return r;
    }
    double x = r < 1.0 ? 1.0 : r; // monotone from a safe side
    for (int it = 0; it < 40; ++it) {
        // f = x^n - r, f' = n*x^(n-1)
        double xn = 1.0, xn1 = 1.0;
        for (uint32_t k = 0; k < tpr - 1; ++k) {
            xn1 *= x;
        }
        xn = xn1 * x;
        x -= (xn - r) / (double(tpr) * xn1);
    }
    return x;
}

} // namespace

// Song parsing (parse_song_json) lives in audio_json.cpp: it shares
// the AudioLoadError taxonomy and the merged patch map there.

// ---- TrackerPlayer ----

void TrackerPlayer::start(const TrackerSong* song, bool loop) {
    song_ = song;
    loop_ = loop;
    finishing_ = false;
    order_idx_ = 0;
    row_idx_ = 0;
    tick_in_row_ = 0;
    accum_ = 0.0;
    rng_ = 0x5EEDCAFEULL;
    for (uint32_t c = 0; c < kChannels; ++c) {
        Channel& ch = chan_[c];
        ch = Channel{};
        // §8.4 static pans: pulse1 -0.2, pulse2 +0.2, wide 0, noise +0.1
        // (constant-power, §3.1: angle = (pan+1)*pi/4).
        static const float pans[kChannels] = {-0.2f, 0.2f, 0.0f, 0.1f};
        const float angle = (pans[c] + 1.0f) * 0.7853981633974483f;
        ch.gl = lut_cos(angle);
        ch.gr = lut_sin(angle);
    }
}

void TrackerPlayer::stop() {
    song_ = nullptr;
    for (Channel& ch : chan_) {
        ch.sounding = false;
        ch.fade_pos = kFadeSamples;
    }
}

uint32_t TrackerPlayer::rng_next() {
    // PCG32 (05-engine-core.md): 64-bit LCG step + XSL-RR output.
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
    const uint32_t xorshifted = uint32_t(((rng_ >> 18u) ^ rng_) >> 27u);
    const uint32_t rot = uint32_t(rng_ >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

float TrackerPlayer::osc(const TrackerInstr& instr, double phase, float noise_val) {
    switch (instr.wave) {
    case Wave::Square:
        return phase < double(instr.duty) ? 0.5f : -0.5f;
    case Wave::Saw:
        return float(1.0 - 2.0 * phase);
    case Wave::Sine:
        return lut_sin(float(kTwoPi) * float(phase));
    case Wave::Triangle:
        return phase < 0.5 ? float(4.0 * phase - 1.0) : float(3.0 - 4.0 * phase);
    case Wave::Noise:
        return noise_val;
    }
    return 0.0f;
}

void TrackerPlayer::process_row(uint32_t row) {
    // Resolve the CURRENT pattern from the order list.
    const std::string& name = song_->order[order_idx_];
    const TrackerPattern* p = nullptr;
    for (const auto& [pname, pattern] : song_->patterns) {
        if (pname == name) {
            p = &pattern;
            break;
        }
    }
    // parse_song_json guarantees every order entry names a pattern.
    if (p == nullptr) {
        stop();
        return;
    }
    for (uint32_t c = 0; c < kChannels; ++c) {
        Channel& ch = chan_[c];
        // Row-scope fx from the PREVIOUS row end here (§8.3 "its row
        // only"); vibrato persists (until note end or replacement).
        ch.slide_this_row = false;
        ch.arp_this_row = false;
        ch.arp_n = 0;
        const TrackerCell& cell = p->chan[c][row];

        // Capture the tail's channel volume BEFORE this row's cell
        // replaces it (the retrigger fade must sound at the level the
        // previous note was actually at; cycle-9 review).
        ch.old_vol_gain = ch.vol_gain;
        if (cell.inst != 0xFF && cell.inst < song_->instruments.size()) {
            ch.instr = &song_->instruments[cell.inst];
        }
        if (cell.vol != 0xFF) {
            const float v = float(cell.vol) / 15.0f;
            ch.vol_gain = v * v; // (vol/15)^2 (§8.3)
        }
        if (cell.note == TrackerCell::kOff) {
            if (ch.sounding && !ch.releasing) {
                // Release from wherever the envelope stands (§8.1).
                const float cur = ch.attack_pos < ch.attack_len
                                      ? float(ch.attack_pos) / float(ch.attack_len)
                                      : 1.0f;
                ch.release_start = std::min(cur, 1.0f);
                ch.releasing = true;
                ch.release_pos = 0;
            }
        } else if (cell.note != 0) {
            const int midi = int(cell.note) + 11;
            const float f = midi_to_freq(midi);
            if (ch.instr == nullptr || f <= 0.0f) {
                continue; // parse guarantees first notes carry inst
            }
            // New note: hard-retrigger — phase reset with a 64-sample
            // fade of the previous signal mixed under (§8.1).
            if (ch.sounding) {
                ch.fade_pos = 0;
                ch.old_phase = ch.phase;
                ch.old_f_used = ch.f_used;
                ch.old_instr = ch.instr;
                // Fade the previous signal out from its CURRENT
                // envelope level — a flat 1.0/release_start would jump
                // the tail up to peak before the 64-sample fade (the
                // click §8.1's fade exists to prevent; cycle-8 review).
                if (ch.releasing) {
                    const float rel = ch.release_len > 0
                                          ? 1.0f - float(ch.release_pos) / float(ch.release_len)
                                          : 0.0f;
                    ch.old_gain = ch.release_start * rel;
                } else {
                    ch.old_gain =
                        ch.attack_len > 0 ? float(ch.attack_pos) / float(ch.attack_len) : 1.0f;
                }
            }
            ch.sounding = true;
            ch.f = f;
            ch.f_used = f;
            ch.phase = 0.0;
            ch.attack_pos = 0;
            ch.attack_len = uint32_t(ch.instr->attack * kSampleRate);
            ch.release_len = uint32_t(ch.instr->release * kSampleRate);
            ch.release_pos = 0;
            ch.release_start = 1.0f;
            ch.releasing = false;
            ch.env_off = false;
            ch.vib_active = false; // note end clears vibrato (§8.3)
            ch.vib_phase = 0.0f;
            ch.noise_acc = 0.0;
        }
        switch (cell.fx) {
        case 1: // A<hex>: arp on this row's ticks
            ch.arp_this_row = true;
            ch.arp_n = cell.arp_n;
            ch.arp = cell.arp;
            break;
        case 2: { // S±n: spread 2^(±n/12) evenly over the row's ticks.
            // semitone_ratio(negative n) is already < 1 — the tpr-th
            // root of it slides DOWN per tick. (A second inversion
            // here made every downward slide climb; cycle-1 review.)
            ch.slide_this_row = true;
            ch.slide_per_tick =
                float(nth_root(double(semitone_ratio(cell.slide)), song_->ticks_per_row));
            break;
        }
        case 3: // V<d>,<s>
            ch.vib_active = true;
            ch.vib_depth = float(cell.vib_depth);
            ch.vib_speed = float(cell.vib_speed);
            break;
        default:
            break;
        }
    }
}

void TrackerPlayer::apply_tick() {
    for (uint32_t c = 0; c < kChannels; ++c) {
        Channel& ch = chan_[c];
        if (!ch.sounding) {
            continue;
        }
        if (ch.slide_this_row) {
            ch.f *= ch.slide_per_tick; // persists past the row (§8.3)
        }
        float f = ch.f;
        if (ch.arp_this_row && ch.arp_n > 0) {
            const uint8_t d = ch.arp[tick_in_row_ % ch.arp_n];
            f *= semitone_ratio(int(d));
        } else if (ch.vib_active && ch.vib_depth > 0.0f) {
            ch.vib_phase += ch.vib_speed * float(kTwoPi) / 64.0f;
            // Wrap keeps long-held notes precise; the per-tick
            // increment is < 2*pi, so the while fully bounds it.
            while (ch.vib_phase >= float(kTwoPi)) {
                ch.vib_phase -= float(kTwoPi);
            }
            // §8.3: f_used = f * 2^((d/8)*sin(phase)/12). The ratio
            // helper computes 2^(x/12), so x = (d/8)*sin(phase) — the
            // depth is in eighth-semitones.
            f *= semitone_ratio_frac((ch.vib_depth / 8.0f) * lut_sin(ch.vib_phase));
        }
        ch.f_used = f <= 0.0f ? 0.0f : f;
    }
}

float TrackerPlayer::channel_sample(Channel& ch) {
    float out = 0.0f;
    // Retrigger fade of the previous signal (§8.1): 64 samples, linear.
    if (ch.fade_pos < kFadeSamples && ch.old_instr != nullptr) {
        const float g = 1.0f - float(ch.fade_pos) / float(kFadeSamples);
        ch.old_phase += double(ch.old_f_used) / double(kSampleRate);
        if (ch.old_phase >= 1.0) {
            ch.old_phase -= 1.0;
        }
        // The envelope model applies to noise exactly as to tonal
        // waves (the main path multiplies env in identically), so the
        // fade uses the captured level for every wave.
        out += osc(*ch.old_instr, ch.old_phase, ch.noise_val) * g * ch.old_vol_gain *
               ch.old_instr->gain * ch.old_gain;
        ++ch.fade_pos;
    }
    if (!ch.sounding || ch.env_off || ch.instr == nullptr) {
        return out;
    }
    // Envelope (linear; §8.1 via §5.1-mapped seconds).
    float env = 1.0f;
    if (ch.releasing) {
        if (ch.release_len == 0) {
            ch.env_off = true;
            ch.sounding = false;
            return out;
        }
        env = ch.release_start * (1.0f - float(ch.release_pos) / float(ch.release_len));
        if (++ch.release_pos >= ch.release_len) {
            ch.env_off = true;
            ch.sounding = false;
        }
    } else if (ch.attack_pos < ch.attack_len) {
        env = float(ch.attack_pos) / float(ch.attack_len);
        ++ch.attack_pos;
        if (ch.attack_pos >= ch.attack_len && ch.instr->sustain <= 0.0f) {
            // One-shot: attack -> immediate release (§8.1).
            ch.releasing = true;
            ch.release_pos = 0;
            ch.release_start = 1.0f;
        }
    } else if (ch.instr->sustain <= 0.0f) {
        // Zero-length attack one-shot: already releasing via the branch
        // above (attack_len 0 -> attack_pos 0 == attack_len). Guard for
        // the pathological case both are zero.
        ch.releasing = true;
        ch.release_pos = 0;
        ch.release_start = 1.0f;
    }
    // gated sustain>0: hold at 1.0 until note end — env stays 1.0.
    if (ch.instr->wave == Wave::Noise) {
        // Sample-and-hold: new PCG32 value every 48000/(8*f) samples
        // (§8.1) — note pitch controls noise color.
        const double hold = ch.f_used > 0.0f ? double(kSampleRate) / (8.0 * double(ch.f_used))
                                             : double(kSampleRate);
        ch.noise_acc += 1.0;
        if (ch.noise_acc >= hold) {
            ch.noise_acc -= hold;
            ch.noise_val = float(int(rng_next() >> 8) - 0x800000) * (1.0f / 8388608.0f);
        }
    }
    ch.phase += double(ch.f_used) / double(kSampleRate);
    if (ch.phase >= 1.0) {
        ch.phase -= 1.0;
        if (ch.phase >= 1.0) {
            ch.phase = std::fmod(ch.phase, 1.0);
        }
    }
    out += osc(*ch.instr, ch.phase, ch.noise_val) * env * ch.vol_gain * ch.instr->gain;
    return out;
}

void TrackerPlayer::render(float* dst, const float* gains, uint32_t frames) {
    if (song_ == nullptr) {
        return;
    }
    const double spt = song_->samples_per_tick();
    uint32_t done = 0;
    while (done < frames) {
        if (accum_ < 1.0) {
            if (!finishing_ && tick_in_row_ == 0) {
                process_row(row_idx_);
                if (song_ == nullptr) {
                    // Defensive stop() (unreachable with validated
                    // songs). render ACCUMULATES into dst: returning
                    // early leaves the remaining frames un-added,
                    // which is exactly silence — nothing to fill.
                    return;
                }
            }
            apply_tick();
            ++tick_in_row_;
            if (tick_in_row_ >= song_->ticks_per_row) {
                tick_in_row_ = 0;
                ++row_idx_;
                if (row_idx_ >= song_->rows_per_pattern()) {
                    row_idx_ = 0;
                    ++order_idx_;
                    if (order_idx_ >= song_->order.size()) {
                        if (loop_) {
                            order_idx_ = 0;
                        } else {
                            // Non-loop end: release everything; the
                            // tail rings out, then the player idles.
                            finishing_ = true;
                            for (Channel& ch : chan_) {
                                // Row-scope fx ended with the song's
                                // last row — hold the tail pitch steady
                                // (§8.3 "its row only"; cycle-8 review).
                                ch.slide_this_row = false;
                                ch.arp_this_row = false;
                                ch.arp_n = 0;
                                ch.vib_active = false; // tail holds pitch
                                if (ch.sounding && !ch.releasing) {
                                    // Release from wherever the envelope
                                    // stands — same rule as note-off
                                    // (§8.1; a mid-attack channel must
                                    // not jump to 1.0, cycle-14 review).
                                    ch.release_start =
                                        ch.attack_pos < ch.attack_len
                                            ? float(ch.attack_pos) / float(ch.attack_len)
                                            : 1.0f;
                                    ch.releasing = true;
                                    ch.release_pos = 0;
                                }
                            }
                        }
                    }
                }
            }
            accum_ += spt;
        }
        const uint32_t n = std::min(frames - done, uint32_t(accum_));
        // spt >= 48000*2.5/260 = 461.5 at the bpm cap, so n >= 1
        // whenever frames remain — the loop always advances.
        for (uint32_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            for (uint32_t c = 0; c < kChannels; ++c) {
                const float s = channel_sample(chan_[c]);
                l += s * chan_[c].gl;
                r += s * chan_[c].gr;
            }
            const float g = gains[done + i];
            dst[2 * (done + i)] += l * g;
            dst[2 * (done + i) + 1] += r * g;
        }
        accum_ -= n;
        done += n;
    }
    if (finishing_) {
        bool any = false;
        for (Channel& ch : chan_) {
            // Same guard as channel_sample's fade path: a fade only
            // counts with an old_instr behind it (cycle-8 review).
            if (ch.sounding || (ch.old_instr != nullptr && ch.fade_pos < kFadeSamples)) {
                any = true;
            }
        }
        if (!any) {
            song_ = nullptr; // inactive: the tail fully rang out
        }
    }
}

float tracker_eq_pow(float x01) {
    return lut_sin(x01 * 1.5707963267948966f);
}

} // namespace tb::audio
