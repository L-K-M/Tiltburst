#include "audio/sfx_synth.h"

#include "core/hash.h"
#include "core/log.h"
#include "core/rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tb::audio {

namespace {

// Classic sfxr constants (12-audio.md §5.2) — the generator runs at
// 44100 Hz so well-known parameter intuitions transfer.
constexpr int kGenRate = 44100;
constexpr int kOutRate = 48000;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kHardCapSamples = 44100 * 4; // 4.0 s (§5.3)

enum EnvStage : int { ATTACK = 0, SUSTAIN, DECAY, ENV_STAGES };

float signf(float v) {
    return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f);
}

struct GenState {
    // Oscillator program (reset by reset_run).
    double fperiod = 0.0;
    double fslide = 0.0;
    double fdslide = 0.0;
    double square_duty = 0.0;
    double duty_slide = 0.0;
    int arp_limit = 0;
    int arp_time = 0;
    bool arp_done = false;
    double arp_mult = 1.0;
    int rep_limit = 0;
    int rep_time = 0;

    // Continuous state — NOT reset by reset_run (§5.3).
    double vib_phase = 0.0;
    double vib_inc = 0.0;
    double vib_amp = 0.0;
    double flanger_phase = 0.0;
    double flanger_dphase = 0.0;
    double fltw = 0.0;
    double fltw_d = 0.0;
    double fltdmp = 0.0;
    double flthp = 0.0;
    double flthp_d = 0.0;
    int phase = 0;
    float flanger_buf[1024] = {}; // per-render: deterministic across
                                  // consecutive renders of the same id
    double fltp = 0.0;
    double fltdp = 0.0;
    double fltphp = 0.0;
    int ipos = 0;
    float noise_buf[32] = {};

    int env_len[ENV_STAGES] = {};
    int env_time = 0;
    int env_stage = 0;

    Pcg32 rng; // §5.3: seeded from the patch id hash — one stream for
               // the initial noise table and every mid-render refill.

    double fmaxperiod = 0.0;
    float freq_limit_param = 0.0f;
    Wave wave = Wave::Square;
    bool lpf_active = true;

    void reset_run(const SfxPatch& p) {
        fperiod = 100.0 / (double(p.base_freq) * double(p.base_freq) + 0.001);
        fslide = 1.0 - double(p.freq_slide) * double(p.freq_slide) * double(p.freq_slide) * 0.01;
        fdslide = -double(p.freq_delta_slide) * double(p.freq_delta_slide) *
                  double(p.freq_delta_slide) * 1e-6;
        square_duty = double(p.duty);
        duty_slide = -double(p.duty_sweep) * 5e-5;
        arp_time = 0;
        arp_done = false;
    }
};

void init_state(GenState& g, const SfxPatch& p, const std::string& id) {
    g.wave = p.wave;
    g.freq_limit_param = p.freq_limit;
    g.fmaxperiod = 100.0 / (double(p.freq_limit) * double(p.freq_limit) + 0.001);

    g.vib_amp = double(p.vib_depth) * 0.5;
    g.vib_inc = double(p.vib_speed) * double(p.vib_speed) * 0.01;

    g.arp_mult = p.arp_mod >= 0.0f ? 1.0 - double(p.arp_mod) * double(p.arp_mod) * 0.9
                                   : 1.0 + double(p.arp_mod) * double(p.arp_mod) * 10.0;
    g.arp_limit =
        p.arp_mod == 0.0f
            ? 0
            : int((1.0 - double(p.arp_speed)) * (1.0 - double(p.arp_speed)) * 20000.0 + 32.0);
    g.rep_limit =
        p.repeat_speed == 0.0f
            ? 0
            : int((1.0 - double(p.repeat_speed)) * (1.0 - double(p.repeat_speed)) * 20000.0 + 32.0);

    g.env_len[ATTACK] = int(double(p.attack) * double(p.attack) * 100000.0);
    g.env_len[SUSTAIN] = int(double(p.sustain) * double(p.sustain) * 100000.0);
    g.env_len[DECAY] = int(double(p.decay) * double(p.decay) * 100000.0);

    g.flanger_phase = double(signf(p.flanger_offset)) * double(p.flanger_offset) *
                      double(p.flanger_offset) * 1020.0;
    g.flanger_dphase =
        double(signf(p.flanger_sweep)) * double(p.flanger_sweep) * double(p.flanger_sweep) * 1.0;

    g.fltw = double(p.lpf_cutoff) * double(p.lpf_cutoff) * double(p.lpf_cutoff) * 0.1;
    g.fltw_d = 1.0 + double(p.lpf_sweep) * 1e-4;
    g.fltdmp = std::clamp(5.0 / (1.0 + double(p.lpf_resonance) * double(p.lpf_resonance) * 20.0) *
                              (0.01 + g.fltw),
                          0.0,
                          0.8);
    g.flthp = double(p.hpf_cutoff) * double(p.hpf_cutoff) * 0.1;
    g.flthp_d = 1.0 + double(p.hpf_sweep) * 3e-4;
    g.lpf_active = p.lpf_cutoff < 1.0f;

    // §5.3: the noise RNG is a PCG32 seeded with the FNV-1a hash of
    // the patch id string, so pre-rendering is reproducible run to run.
    const uint64_t seed = fnv1a64(id.data(), id.size(), 0x9EED5EEDull);
    g.rng.seed(seed, 0xDA7A5EEDull);
    for (float& n : g.noise_buf) {
        n = g.rng.next_float() * 2.0f - 1.0f;
    }
    g.reset_run(p);
}

inline float waveform(const GenState& g, double fp) {
    switch (g.wave) {
    case Wave::Square:
        return fp < g.square_duty ? 0.5f : -0.5f;
    case Wave::Saw:
        return float(1.0 - 2.0 * fp);
    case Wave::Sine:
        return float(std::sin(2.0 * kPi * fp));
    case Wave::Triangle:
        return fp < 0.5 ? float(4.0 * fp - 1.0) : float(3.0 - 4.0 * fp);
    case Wave::Noise:
        return g.noise_buf[int(fp * 32.0) & 31];
    }
    return 0.0f;
}

} // namespace

float db_to_amp(float db) {
    return float(std::pow(10.0, double(db) / 20.0));
}

bool render_patch(const SfxPatch& patch, const std::string& patch_id, std::vector<float>& out_pcm) {
    out_pcm.clear();

    GenState g;
    init_state(g, patch, patch_id);

    std::vector<float> gen;
    gen.reserve(size_t(patch.sustain + patch.decay) * 44100u + 44100u);

    for (uint32_t t = 0; t < kHardCapSamples; ++t) {
        // --- pitch program (per output sample) ---
        if (g.rep_limit > 0 && ++g.rep_time >= g.rep_limit) {
            g.rep_time = 0;
            g.reset_run(patch);
        }
        if (g.arp_limit > 0 && !g.arp_done && ++g.arp_time >= g.arp_limit) {
            g.arp_done = true;
            g.fperiod *= g.arp_mult;
        }
        g.fslide += g.fdslide;
        g.fperiod *= g.fslide;
        if (g.fperiod > g.fmaxperiod) {
            g.fperiod = g.fmaxperiod;
            if (g.freq_limit_param > 0.0f) {
                break; // pitch fell below freq_limit: sound ends
            }
        }
        g.vib_phase += g.vib_inc;
        const double rfperiod = g.fperiod * (1.0 + g.vib_amp * std::sin(g.vib_phase));
        const int period = std::max(8, int(rfperiod));
        g.square_duty = std::clamp(g.square_duty + g.duty_slide, 0.02, 0.98);

        // --- envelope (3-stage; the while-skip advances through
        // zero-length stages, so env_len[stage] > 0 whenever evaluated) ---
        ++g.env_time;
        while (g.env_stage < ENV_STAGES && g.env_time > g.env_len[g.env_stage]) {
            g.env_time = 0;
            ++g.env_stage;
        }
        if (g.env_stage > DECAY) {
            break;
        }
        double env_vol = 0.0;
        if (g.env_stage == ATTACK) {
            env_vol = double(g.env_time) / double(g.env_len[ATTACK]);
        } else if (g.env_stage == SUSTAIN) {
            env_vol = 1.0 + (1.0 - double(g.env_time) / double(g.env_len[SUSTAIN])) * 2.0 *
                                double(patch.punch);
        } else {
            env_vol = 1.0 - double(g.env_time) / double(g.env_len[DECAY]);
        }

        // --- flanger delay line ---
        g.flanger_phase += g.flanger_dphase;
        const int iphase = std::clamp(int(std::fabs(g.flanger_phase)), 0, 1023);

        // --- 8x supersampled synthesis ---
        double ssample = 0.0;
        for (int sub = 0; sub < 8; ++sub) {
            ++g.phase;
            if (g.phase >= period) {
                g.phase %= period;
                if (g.wave == Wave::Noise) {
                    for (float& n : g.noise_buf) {
                        n = g.rng.next_float() * 2.0f - 1.0f;
                    }
                }
            }
            const double fp = double(g.phase) / double(period);
            const double raw = waveform(g, fp);

            const double prev_fltp = g.fltp;
            g.fltw = std::clamp(g.fltw * g.fltw_d, 0.0, 0.1);
            if (g.lpf_active) {
                g.fltdp += (raw - g.fltp) * g.fltw;
                g.fltdp -= g.fltdp * g.fltdmp;
            } else {
                g.fltp = raw;
                g.fltdp = 0.0;
            }
            g.fltp += g.fltdp;

            g.flthp = std::clamp(g.flthp * g.flthp_d, 1e-5, 0.1);
            g.fltphp += g.fltp - prev_fltp;
            g.fltphp -= g.fltphp * g.flthp;
            double s = g.fltphp;

            g.flanger_buf[size_t(g.ipos) & 1023] = float(s);
            s += g.flanger_buf[size_t(g.ipos - iphase + 1024) & 1023];
            ++g.ipos;
            ssample += s;
        }
        const float sample = float(std::clamp((ssample / 8.0) * env_vol, -8.0, 8.0));
        gen.push_back(sample);
    }

    if (gen.size() >= kHardCapSamples) {
        TB_LOG_WARN("audio", "patch '{}' hit the 4 s render cap", patch_id);
    }

    // §5.4: peak-normalize to -1 dBFS, then volume_db.
    float peak = 0.0f;
    for (float s : gen) {
        peak = std::max(peak, std::fabs(s));
    }
    if (peak <= 1e-4f) {
        return false; // all-zero clip is a tb_validate error
    }
    const float norm = 0.891f / peak;

    // Linear resample 44100 -> 48000.
    const size_t out_len = size_t(uint64_t(gen.size()) * kOutRate / kGenRate);
    out_pcm.resize(out_len);
    const double step = double(kGenRate) / double(kOutRate);
    double pos = 0.0;
    for (size_t i = 0; i < out_len; ++i, pos += step) {
        const size_t i0 = size_t(pos);
        const size_t i1 = std::min(i0 + 1, gen.size() - 1);
        const double frac = pos - double(i0);
        out_pcm[i] =
            float((double(gen[i0]) * (1.0 - frac) + double(gen[i1]) * frac) * double(norm));
    }
    return true;
}

} // namespace tb::audio
