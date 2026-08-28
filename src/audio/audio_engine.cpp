#include "audio/audio_engine.h"

#include "core/log.h"
#include "core/time.h"
#include "miniaudio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tb::audio {

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kVoiceCount = 32;  // §3.1
constexpr uint32_t kPendingCap = 64;  // §4.2
constexpr uint32_t kFadeSamples = 64; // §3.2 steal fade (1.33 ms)
constexpr uint32_t kGainRamp = 960;   // §3.1 bus volume ramp (20 ms)
constexpr uint32_t kMaxFrames = 512;  // ladder floor; offline render clamps
constexpr uint32_t kBusCount = 4;     // sfx, ui, music, master
constexpr double kPi = 3.14159265358979323846;

// §3.3 limiter constants (exact).
constexpr double kLimT = 0.891;
constexpr double kLimK = 1.5;
const double kLimInvK = 1.0 / std::tanh(kLimK);
const double kLimAtt = std::exp(-1.0 / (0.002 * 48000.0));
const double kLimRel = std::exp(-1.0 / (0.150 * 48000.0));

constexpr uint32_t kFlipperPatchId = 0; // §7.1 id order; latency pairing

struct Voice {
    const float* pcm = nullptr;
    uint32_t len = 0;
    uint32_t pos = 0;         // next PCM index to emit
    uint32_t start_frame = 0; // frame within THIS buffer playback begins
    float gain = 1.0f;        // velocity * patch gain
    float gl = 1.0f, gr = 0.0f;
    uint8_t bus = 0; // 0 sfx, 1 ui
    uint8_t priority = 5;
    uint16_t patch = 0;
    uint64_t seq = 0;
    bool active = false;
};

struct Pending {
    uint64_t sample = 0;
    SoundEvent ev{};
};

inline float gain_from_volume(int v) {
    const float f = std::clamp(v, 0, 100) / 100.0f;
    return f * f; // §3.1
}

} // namespace

struct AudioEngineImpl {
    ma_context* ctx = nullptr;
    ma_device* device = nullptr;
    bool running = false;

    static constexpr uint32_t kSoundCapLog = 10; // 1024 (§2.3)
    SoundEventQueue sounds{kSoundCapLog};
    CommandQueue commands{6}; // 64 (§2.3)

    // Bank publish (§2.3): pointer + epoch; the previous bank is
    // retired and freed by pump() once the callback acks the epoch.
    std::atomic<PatchBank*> bank{nullptr};
    std::atomic<uint64_t> publish_epoch{0};
    PatchBank* retired = nullptr;
    uint64_t retired_epoch = 0;

    // §4.2 clock (audio thread only).
    double spt = 48.0;
    uint64_t anchor_tick = 0;
    double anchor_sample = 0.0;
    double d_avg = 0.0;
    bool clock_init = false;
    uint64_t stream_pos = 0;
    uint32_t lead = 192; // D = P + 64, set at init

    Voice voices[kVoiceCount];
    uint64_t next_seq = 1;
    Pending pending[kPendingCap];
    uint32_t pending_n = 0;

    // Bus ramp state: current -> target, walked 960 frames (§3.1).
    float bus_target[kBusCount] = {1.0f, 1.0f, 1.0f, 1.0f};
    float bus_gain[kBusCount] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Preallocated mixing scratch (callback is allocation-free).
    float bus_mix[3][kMaxFrames * 2] = {}; // sfx, ui, music
    float frame_gain[kBusCount][kMaxFrames] = {};

    // §3.3 limiter envelope persists across callbacks.
    double lim_env = 0.0;

    // §2.1 underrun ladder bookkeeping + §12 callback entry timestamp.
    uint64_t prev_cb_ns = 0;
    uint64_t started_ns = 0;
    uint64_t cb_wall_ns = 0;

    // §12 latency probes.
    struct Probe {
        uint64_t t0_ns;
        uint32_t tick;
    };

    Probe probes[64];
    std::atomic<uint64_t> probe_head{0};
    std::atomic<uint64_t> probe_tail{0};
    std::atomic<uint32_t> lat_us[256]; // micro-integer ms*1000: atomic
                                       // reads are torn-free across threads
    std::atomic<uint32_t> lat_n{0};
    uint32_t lat_write = 0;
    bool latency_probe_on = false;

    // Callback CPU accounting.
    uint64_t cb_cpu_ns = 0;
    uint64_t cb_frames = 0;

    // Test-only start log (audio thread writes; offline tests read).
    AudioSystem::DebugStart debug_starts[AudioSystem::kDebugStarts] = {};
    uint32_t debug_write = 0;
    uint64_t stream_pos_at_mix_start = 0;
};

// ---- voice pool (§3.2): free functions over Impl ----

namespace {

// Oldest active voice playing `patch`.
int find_same_patch(const Voice* voices, uint16_t patch) {
    int found = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < int(kVoiceCount); ++i) {
        if (voices[i].active && voices[i].patch == patch && voices[i].seq < oldest) {
            oldest = voices[i].seq;
            found = i;
        }
    }
    return found;
}

int count_same_patch(const Voice* voices, uint16_t patch) {
    int n = 0;
    for (int i = 0; i < int(kVoiceCount); ++i) {
        if (voices[i].active && voices[i].patch == patch) {
            ++n;
        }
    }
    return n;
}

// Lowest priority, then oldest seq (§3.2); -1 when none active.
int find_steal_candidate(const Voice* voices) {
    int found = -1;
    uint8_t best_prio = 255;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < int(kVoiceCount); ++i) {
        const Voice& v = voices[i];
        if (!v.active) {
            continue;
        }
        if (v.priority < best_prio || (v.priority == best_prio && v.seq < oldest)) {
            best_prio = v.priority;
            oldest = v.seq;
            found = i;
        }
    }
    return found;
}

// §3.2: cut with a 64-sample linear fade mixed into the current buffer.
void steal_fade(AudioEngineImpl& impl, const Voice& v) {
    // Only voices that actually sounded get a fade (pos > 0): a voice
    // stolen before its scheduled start_frame has emitted nothing.
    if (v.pos == 0) {
        return;
    }
    const uint32_t bus = v.bus < 2 ? v.bus : 0; // same clamp as the mix
    const uint32_t n = std::min<uint32_t>(kFadeSamples, v.len - v.pos);
    for (uint32_t i = 0; i < n; ++i) {
        const float g = 1.0f - float(i) / float(kFadeSamples);
        const float s = v.pcm[v.pos + i] * v.gain * g;
        impl.bus_mix[bus][2 * i] += s * v.gl;
        impl.bus_mix[bus][2 * i + 1] += s * v.gr;
    }
}

// §12: pair a flipper voice start with the freshest {t0, tick} probe.
void record_latency_probe(AudioEngineImpl& impl, const SoundEvent& ev, int period, int periods) {
    if (!impl.latency_probe_on || ev.patch != kFlipperPatchId || impl.cb_wall_ns == 0) {
        return;
    }
    const uint64_t head = impl.probe_head.load(std::memory_order_relaxed);
    uint64_t tail = impl.probe_tail.load(std::memory_order_acquire);
    while (tail != head) {
        const AudioEngineImpl::Probe& p = impl.probes[tail & 63];
        const uint64_t next = tail + 1;
        if (p.tick == ev.tick) {
            const double t_dac = double(impl.cb_wall_ns) +
                                 (double(uint32_t(period * periods)) / double(kRate)) * 1e9;
            const double delta_ms = (t_dac - double(p.t0_ns)) / 1e6;
            if (delta_ms >= 0.0 && delta_ms < 1000.0) {
                impl.lat_us[impl.lat_write & 255].store(uint32_t(delta_ms * 1000.0),
                                                        std::memory_order_relaxed);
                impl.lat_write++;
                impl.lat_n.fetch_add(1, std::memory_order_relaxed);
            }
            impl.probe_tail.store(next, std::memory_order_release);
            return;
        }
        if (p.tick > ev.tick) {
            return; // future probe for a later event: keep it queued
        }
        impl.probe_tail.store(next, std::memory_order_release); // stale: drop
        tail = next;
    }
}

// Acquire a voice for the event; -1 = dropped (§3.2 rules).
int acquire_voice(AudioEngineImpl& impl,
                  AudioStats& stats,
                  const PatchBank& bank,
                  const SoundEvent& ev,
                  uint32_t start_frame,
                  int period,
                  int periods) {
    if (ev.patch >= bank.entries.size()) {
        return -1;
    }
    const PatchEntry& entry = bank.entries[ev.patch];
    if (entry.pcm.empty()) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < int(kVoiceCount); ++i) {
        if (!impl.voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        const int cand = find_steal_candidate(impl.voices);
        if (cand < 0 || entry.priority < impl.voices[cand].priority) {
            stats.dropped_events.fetch_add(1, std::memory_order_relaxed);
            return -1; // new sound loses
        }
        slot = cand;
    } else if (count_same_patch(impl.voices, ev.patch) >= 4) {
        // Per-patch cap: steal the oldest voice of THIS patch.
        const int cand = find_same_patch(impl.voices, ev.patch);
        if (cand >= 0) {
            slot = cand;
        }
    }

    Voice& v = impl.voices[slot];
    if (v.active) {
        steal_fade(impl, v);
        stats.stolen_voices.fetch_add(1, std::memory_order_relaxed);
    }

    v.pcm = entry.pcm.data();
    v.len = uint32_t(entry.pcm.size());
    v.pos = 0;
    v.start_frame = start_frame;
    v.gain = (0.25f + 0.75f * std::clamp(ev.velocity, 0.0f, 1.0f)) * entry.gain;
    const float angle = (std::clamp(ev.pan, -1.0f, 1.0f) + 1.0f) * (float(kPi) * 0.25f);
    v.gl = std::cos(angle); // constant-power pan (§3.1)
    v.gr = std::sin(angle);
    // Event buses are 0 (sfx) / 1 (ui); 2 (music) is tracker-only
    // (M14) and 3 is reserved — both clamp to sfx with the flags
    // masked so no out-of-range bus index can exist.
    v.bus = ((ev.flags >> 1) & 0x3) >= 2 ? 0 : uint8_t((ev.flags >> 1) & 0x3);
    v.priority = entry.priority;
    v.patch = ev.patch;
    v.seq = impl.next_seq++;
    v.active = true;
    // Test evidence: absolute start sample (stream pos at buffer start
    // + the in-buffer offset).
    impl.debug_starts[impl.debug_write & (AudioSystem::kDebugStarts - 1)] = {
        ev.patch, 0, impl.stream_pos_at_mix_start + start_frame};
    impl.debug_write++;
    record_latency_probe(impl, ev, period, periods);
    return slot;
}

} // namespace

AudioSystem::AudioSystem() : impl_(new AudioEngineImpl()) {
    sounds_ = &impl_->sounds;
}

AudioSystem::~AudioSystem() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

void AudioSystem::publish_tick(uint64_t tick) {
    latest_tick_.store(tick, std::memory_order_relaxed);
}

void AudioSystem::push_latency_probe(uint64_t t0_ns, uint32_t tick) {
    if (!impl_->latency_probe_on) {
        return;
    }
    const uint64_t head = impl_->probe_head.load(std::memory_order_relaxed);
    const uint64_t tail = impl_->probe_tail.load(std::memory_order_acquire);
    if (head - tail >= 64) {
        return; // drop new
    }
    impl_->probes[head & 63] = {t0_ns, tick};
    impl_->probe_head.store(head + 1, std::memory_order_release);
}

bool AudioSystem::push_command(const AudioCommand& cmd) {
    return impl_->commands.push(cmd);
}

void AudioSystem::publish_bank(std::unique_ptr<PatchBank> bank) {
    PatchBank* next = bank.release();
    PatchBank* old = impl_->bank.exchange(next, std::memory_order_acq_rel);
    const uint64_t epoch = impl_->publish_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (old != nullptr) {
        if (impl_->retired != nullptr) {
            // Prior retire never acked (device dead or callback idle):
            // KEEP BOTH in the retire chain — an eager delete could
            // free a bank a mid-mix callback still holds (cycle-1
            // blocker). The chain leaks at most until shutdown, which
            // frees everything.
            PatchBank* first = impl_->retired;
            impl_->retired = old;
            old->retire_next = first; // intrusive singly-linked list
        } else {
            impl_->retired = old;
            old->retire_next = nullptr;
        }
        impl_->retired_epoch = epoch;
    }
}

void AudioSystem::pump() {
    // The callback acks at MIX EXIT: any bank in the chain is either
    // the pre-swap mix target (acked only after that mix finished) or
    // older. Free the whole chain once the newest epoch is acked.
    if (impl_->retired != nullptr &&
        stats_.acked_epoch.load(std::memory_order_acquire) >= impl_->retired_epoch) {
        PatchBank* p = impl_->retired;
        while (p != nullptr) {
            PatchBank* next = p->retire_next;
            delete p;
            p = next;
        }
        impl_->retired = nullptr;
    }
}

float AudioSystem::percentile_ms(int pct) const {
    const uint32_t n = impl_->lat_n.load(std::memory_order_relaxed);
    if (n == 0) {
        return 0.0f;
    }
    double vals[256];
    const uint32_t count = std::min<uint32_t>(n, 256);
    for (uint32_t i = 0; i < count; ++i) {
        vals[i] = double(impl_->lat_us[i].load(std::memory_order_relaxed)) / 1000.0;
    }
    std::sort(vals, vals + count);
    const uint32_t idx = std::min(count - 1, count * uint32_t(pct) / 100u);
    return float(vals[idx]);
}

uint32_t AudioSystem::measured_latency_n() const {
    return impl_->lat_n.load(std::memory_order_relaxed);
}

float AudioSystem::estimated_latency_ms() const {
    return float(double(period_) * double(periods_) / double(kRate) * 1000.0);
}

double AudioSystem::debug_spt() const {
    return impl_->spt;
}

uint64_t AudioSystem::debug_stream_pos() const {
    return impl_->stream_pos;
}

uint32_t AudioSystem::debug_starts(uint32_t count, DebugStart* out) const {
    // The debug ring is written on the audio thread and read from the
    // test thread between renders (single-threaded in offline mode).
    const uint32_t n = std::min<uint32_t>(count, impl_->debug_write);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx = (impl_->debug_write - n + i) & (kDebugStarts - 1);
        out[i] = impl_->debug_starts[idx];
    }
    return n;
}

// ---- device ----

bool AudioSystem::init(const AudioConfig& cfg) {
    shutdown();
    null_backend_ = cfg.null_backend;
    impl_->latency_probe_on = cfg.latency_probe;

    impl_->bus_target[0] = gain_from_volume(cfg.sfx);
    impl_->bus_target[1] = gain_from_volume(cfg.ui);
    impl_->bus_target[2] = gain_from_volume(cfg.music);
    impl_->bus_target[3] = gain_from_volume(cfg.master);
    for (int b = 0; b < 4; ++b) {
        impl_->bus_gain[b] = impl_->bus_target[b];
    }

    // A bank always exists so events resolve even pre-table (§7).
    publish_bank(PatchBank::built_ins());

    if (cfg.null_backend) {
        period_ = 128;
        periods_ = 2;
        impl_->lead = period_ + 64;
        impl_->running = true;
        TB_LOG_INFO("audio",
                    "backend=null rate=48000 period={} periods={} est_latency_ms={:.2f} "
                    "lead_ms=4.00",
                    period_,
                    periods_,
                    estimated_latency_ms());
        return true;
    }

    impl_->ctx = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, impl_->ctx) != MA_SUCCESS) {
        TB_LOG_WARN("audio", "no audio context; audio disabled");
        delete impl_->ctx;
        impl_->ctx = nullptr;
        return false;
    }

    const int ladder[3] = {128, 256, 512};
    const bool direct =
        cfg.period_frames == 128 || cfg.period_frames == 256 || cfg.period_frames == 512;
    bool opened = false;
    for (int attempt = 0; attempt < 3 && !opened; ++attempt) {
        period_ = direct ? cfg.period_frames : ladder[attempt];
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.format = ma_format_f32;
        dc.playback.channels = 2;
        dc.sampleRate = kRate;
        dc.periodSizeInFrames = ma_uint32(period_);
        dc.periods = periods_;
        dc.performanceProfile = ma_performance_profile_low_latency;
        dc.dataCallback = &AudioSystem::dataCallback;
        dc.pUserData = this;
        dc.noPreSilencedOutputBuffer = MA_TRUE;
        impl_->device = new ma_device();
        periods_ = 2; // §2 configuration (fixed)
        // Callback-visible state must be ready BEFORE ma_device_start:
        // the first callback can fire before start() returns.
        impl_->lead = ma_uint32(period_) + 64;
        impl_->running = true;
        impl_->started_ns = tb_now_ns();
        impl_->prev_cb_ns = 0;
        if (ma_device_init(impl_->ctx, &dc, impl_->device) == MA_SUCCESS &&
            ma_device_start(impl_->device) == MA_SUCCESS) {
            opened = true;
        } else {
            // Failed candidate: running was set pre-attempt; a dead
            // ladder step must not claim a live device.
            impl_->running = false;
            ma_device_uninit(impl_->device);
            delete impl_->device;
            impl_->device = nullptr;
            if (direct) {
                break; // persisted size failed once: fall through disabled
            }
        }
    }
    if (!opened) {
        TB_LOG_WARN("audio", "no playable device; audio disabled (null path available)");
        ma_context_uninit(impl_->ctx);
        delete impl_->ctx;
        impl_->ctx = nullptr;
        return false;
    }

    const double lead_ms = double(impl_->lead) / double(kRate) * 1000.0;
    // §2.2 startup line: exactly one, INFO at 128, WARN otherwise.
    if (period_ == 128) {
        TB_LOG_INFO("audio",
                    "backend=device rate=48000 period={} periods={} "
                    "est_latency_ms={:.2f} lead_ms={:.2f}",
                    period_,
                    periods_,
                    estimated_latency_ms(),
                    lead_ms);
    } else {
        TB_LOG_WARN("audio",
                    "backend=device rate=48000 period={} periods={} "
                    "est_latency_ms={:.2f} lead_ms={:.2f}",
                    period_,
                    periods_,
                    estimated_latency_ms(),
                    lead_ms);
    }
    return true;
}

void AudioSystem::shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->running = false;
    if (impl_->device != nullptr) {
        ma_device_stop(impl_->device);
        ma_device_uninit(impl_->device);
        delete impl_->device;
        impl_->device = nullptr;
    }
    if (impl_->ctx != nullptr) {
        ma_context_uninit(impl_->ctx);
        delete impl_->ctx;
        impl_->ctx = nullptr;
    }
    {
        PatchBank* p = impl_->retired;
        while (p != nullptr) {
            PatchBank* next = p->retire_next;
            delete p;
            p = next;
        }
        impl_->retired = nullptr;
    }
    PatchBank* b = impl_->bank.exchange(nullptr, std::memory_order_acq_rel);
    delete b;
}

void AudioSystem::dataCallback(ma_device* device, void* out, const void*, ma_uint32 frames) {
    auto* self = static_cast<AudioSystem*>(device->pUserData);
    const uint64_t t0 = tb_now_ns();
    AudioEngineImpl* impl = self->impl_;
    impl->cb_wall_ns = t0;

    // §2.1 underrun: callback-entry gap > 1.5x period in the first 10 s.
    if (impl->prev_cb_ns != 0 && t0 - impl->started_ns < 10'000'000'000ull) {
        const double period_ns = double(self->period_) * 1e9 / double(kRate);
        if (double(t0 - impl->prev_cb_ns) > 1.5 * period_ns) {
            self->stats_.underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    impl->prev_cb_ns = t0;

    // Devices can request buffers beyond the scratch size: render in
    // chunks so no tail is left unfilled (miniaudio expects a full
    // buffer every callback).
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t chunk = std::min<uint32_t>(frames - done, kMaxFrames);
        self->mix(static_cast<float*>(out) + 2 * size_t(done), chunk);
        done += chunk;
    }

    // CPU%: share of wall time spent inside the callback.
    impl->cb_cpu_ns += tb_now_ns() - t0;
    impl->cb_frames += frames;
    if (impl->cb_frames >= kRate) {
        const double wall_ns = double(impl->cb_frames) * 1e9 / double(kRate);
        const double pct = 100.0 * double(impl->cb_cpu_ns) / wall_ns;
        self->stats_.callback_cpu_pct.store(uint32_t(pct), std::memory_order_relaxed);
        impl->cb_cpu_ns = 0;
        impl->cb_frames = 0;
    }
}

// ---- callback core (also the offline path) ----

void AudioSystem::render_offline(float* out, uint32_t frames) {
    mix(out, std::min<uint32_t>(frames, kMaxFrames));
}

void AudioSystem::mix(float* out, uint32_t frames) {
    frames = std::min<uint32_t>(frames, kMaxFrames);
    PatchBank* bank = impl_->bank.load(std::memory_order_acquire);
    if (bank == nullptr) {
        std::memset(out, 0, sizeof(float) * 2 * size_t(frames));
        return;
    }
    // NOTE: the epoch ack happens at MIX EXIT (below) — an entry ack
    // would bless a publish while this callback still mixes the OLD
    // bank pointer (cycle-1 blocker).

    const uint64_t T = latest_tick_.load(std::memory_order_relaxed);
    const uint64_t pos0 = impl_->stream_pos;
    const uint64_t pos_end = pos0 + frames;
    impl_->stream_pos_at_mix_start = pos0;

    // ---- §4.2 drift-corrected clock ----
    if (!impl_->clock_init) {
        impl_->anchor_tick = T;
        impl_->anchor_sample = double(pos0);
        impl_->clock_init = true;
    }
    const double ideal = impl_->anchor_sample + double(T - impl_->anchor_tick) * impl_->spt;
    const double err = ideal - double(pos0);
    if (std::fabs(err) > 480.0) {
        impl_->anchor_tick = T;
        impl_->anchor_sample = double(pos0);
        impl_->d_avg = 0.0;
    } else {
        impl_->d_avg = 0.98 * impl_->d_avg + 0.02 * err;
        impl_->spt = std::clamp(48.0 * (1.0 - impl_->d_avg / 480000.0), 47.976, 48.024);
    }
    const auto sample_for_tick = [&](uint64_t tick) {
        return impl_->anchor_sample + double(tick - impl_->anchor_tick) * impl_->spt +
               double(impl_->lead);
    };

    // ---- clear the bus accumulators FIRST: steal fades during event
    // processing below write directly into them (§3.2) ----
    for (int b = 0; b < 3; ++b) {
        std::memset(impl_->bus_mix[b], 0, sizeof(float) * 2 * size_t(frames));
    }

    // ---- commands ----
    AudioCommand cmd;
    while (impl_->commands.pop(cmd)) {
        switch (cmd.kind) {
        case AudioCommand::Kind::Volume:
            if (cmd.bus < kBusCount) {
                impl_->bus_target[cmd.bus] = gain_from_volume(int(cmd.value));
            }
            break;
        case AudioCommand::Kind::PlayUi: {
            // UI sounds start at the next buffer start — no tick
            // mapping (§4.1).
            SoundEvent ui;
            ui.patch = cmd.patch;
            ui.flags = 1u << 1; // ui bus
            acquire_voice(*impl_, stats_, *bank, ui, 0, period_, int(periods_));
            break;
        }
        case AudioCommand::Kind::StopAll:
            for (Voice& v : impl_->voices) {
                v.active = false;
            }
            impl_->pending_n = 0;
            break;
        }
    }

    // ---- drain SoundEvents (§4.2 classification) ----
    SoundEvent ev;
    while (impl_->sounds.pop(ev)) {
        // Events older than the anchor (stale ring contents across a
        // re-anchor) would wrap the unsigned tick delta to ~2^64 and
        // wedge a pending slot forever — they are late by definition.
        if (ev.tick < impl_->anchor_tick) {
            stats_.late_events.fetch_add(1, std::memory_order_relaxed);
            acquire_voice(*impl_, stats_, *bank, ev, 0, period_, int(periods_));
            continue;
        }
        const double s = std::round(sample_for_tick(ev.tick));
        if (s < double(pos0)) {
            stats_.late_events.fetch_add(1, std::memory_order_relaxed);
            acquire_voice(*impl_, stats_, *bank, ev, 0, period_, int(periods_));
        } else if (s < double(pos_end)) {
            acquire_voice(
                *impl_, stats_, *bank, ev, uint32_t(s - double(pos0)), period_, int(periods_));
        } else if (impl_->pending_n < kPendingCap) {
            uint32_t i = impl_->pending_n;
            while (i > 0 && impl_->pending[i - 1].sample > uint64_t(s)) {
                impl_->pending[i] = impl_->pending[i - 1];
                --i;
            }
            impl_->pending[i] = Pending{uint64_t(s), ev};
            ++impl_->pending_n;
        } else {
            stats_.late_events.fetch_add(1, std::memory_order_relaxed);
            acquire_voice(*impl_, stats_, *bank, ev, 0, period_, int(periods_));
        }
    }
    // Re-examine pending: anything landing inside this buffer starts now.
    uint32_t still = 0;
    for (uint32_t i = 0; i < impl_->pending_n; ++i) {
        const Pending& p = impl_->pending[i];
        if (p.sample < pos_end) {
            stats_.late_events.fetch_add(1, std::memory_order_relaxed);
            acquire_voice(*impl_,
                          stats_,
                          *bank,
                          p.ev,
                          p.sample < pos0 ? 0u : uint32_t(p.sample - pos0),
                          period_,
                          int(periods_));
        } else {
            impl_->pending[still++] = p;
        }
    }
    impl_->pending_n = still;

    // ---- per-frame bus gain curves (§3.1 ramps) ----
    for (uint32_t b = 0; b < kBusCount; ++b) {
        for (uint32_t i = 0; i < frames; ++i) {
            float& g = impl_->bus_gain[b];
            const float target = impl_->bus_target[b];
            if (g != target) {
                const float step = (target - g) / float(kGainRamp);
                g += step;
                // Snap when within one step of the target.
                if ((step > 0.0f && g >= target) || (step < 0.0f && g <= target)) {
                    g = target;
                }
            }
            impl_->frame_gain[b][i] = g;
        }
    }

    // ---- mix voices into their buses ----
    uint32_t active = 0;
    for (Voice& v : impl_->voices) {
        if (!v.active || v.pcm == nullptr) {
            continue;
        }
        ++active;
        const uint32_t bus = v.bus < 2 ? v.bus : 0;
        float* mixbuf = impl_->bus_mix[bus];
        uint32_t i = std::min(v.start_frame, frames);
        while (i < frames && v.pos < v.len) {
            const float s = v.pcm[v.pos];
            mixbuf[2 * i] += s * v.gl * v.gain;
            mixbuf[2 * i + 1] += s * v.gr * v.gain;
            ++v.pos;
            ++i;
        }
        if (v.pos >= v.len) {
            v.active = false;
        }
    }

    // ---- sum buses (master gain per frame) + limiter (§3.3) ----
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        double l = 0.0, r = 0.0;
        for (int b = 0; b < 3; ++b) {
            const float g = impl_->frame_gain[b][i];
            l += double(impl_->bus_mix[b][2 * i]) * g;
            r += double(impl_->bus_mix[b][2 * i + 1]) * g;
        }
        l *= double(impl_->frame_gain[3][i]);
        r *= double(impl_->frame_gain[3][i]);

        const double peak_lr = std::max(std::fabs(l), std::fabs(r));
        impl_->lim_env = peak_lr > impl_->lim_env
                             ? kLimAtt * impl_->lim_env + (1.0 - kLimAtt) * peak_lr
                             : kLimRel * impl_->lim_env + (1.0 - kLimRel) * peak_lr;
        const double g = impl_->lim_env > kLimT ? kLimT / impl_->lim_env : 1.0;
        const float ol = float(std::tanh(kLimK * g * l) * kLimInvK);
        const float orr = float(std::tanh(kLimK * g * r) * kLimInvK);
        out[2 * i] = ol;
        out[2 * i + 1] = orr;
        peak = std::max(peak, std::fabs(ol));
        peak = std::max(peak, std::fabs(orr));
    }

    impl_->stream_pos = pos_end;
    // §2.3 epoch ack: after the last use of the bank pointer in this
    // mix, so a main-thread free of the retired bank is safe.
    stats_.acked_epoch.store(impl_->publish_epoch.load(std::memory_order_relaxed),
                             std::memory_order_release);
    stats_.active_voices.store(active, std::memory_order_relaxed);
    stats_.peak_master_milli.store(uint32_t(peak * 1000.0f), std::memory_order_relaxed);
}

} // namespace tb::audio
