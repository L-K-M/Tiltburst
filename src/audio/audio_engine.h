#pragma once

#include "audio/audio_bank.h"
#include "sim/sound_out.h"

#include <atomic>
#include <cstdint>
#include <memory>

struct ma_device;
struct ma_context;

namespace tb::audio {

struct AudioEngineImpl; // definition in audio_engine.cpp

using sim::SoundEvent;
using sim::SoundProducer;

static_assert(sizeof(SoundEvent) == 16);

// Drop-NEW single-producer/single-consumer ring (§4.1: the producer
// side full drops the NEW event). Capacity is a power of two.
class SoundEventQueue : public SoundProducer {
public:
    explicit SoundEventQueue(uint32_t capacity_log2 = 10) // 1024 (§2.3)
        : cap_(1u << std::min(capacity_log2, 16u)), mask_(cap_ - 1) {
        buf_ = new SoundEvent[cap_];
    }

    ~SoundEventQueue() { delete[] buf_; }

    SoundEventQueue(const SoundEventQueue&) = delete;
    SoundEventQueue& operator=(const SoundEventQueue&) = delete;

    // sim thread (SoundProducer). Returns false when full.
    bool push(const SoundEvent& ev) override {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= cap_) {
            return false;
        }
        buf_[size_t(head & mask_)] = ev;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // audio thread.
    bool pop(SoundEvent& out) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf_[size_t(tail & mask_)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    SoundEvent* buf_ = nullptr;
    const uint32_t cap_;
    const uint32_t mask_;
    alignas(64) std::atomic<uint64_t> head_{0}; // producer
    alignas(64) std::atomic<uint64_t> tail_{0}; // consumer
};

// main → audio commands (§2.3): volumes ramp; UI sounds start at the
// next buffer (no tick mapping); panic stop.
struct AudioCommand {
    enum class Kind : uint8_t { Volume = 0, PlayUi, StopAll };
    Kind kind = Kind::Volume;
    uint8_t bus = 0;    // 0 sfx, 1 ui, 2 music, 3 master
    float value = 0.0f; // volume 0..100 (Volume) / unused (PlayUi)
    uint16_t patch = 0; // PlayUi patch id
};

class CommandQueue {
public:
    explicit CommandQueue(uint32_t capacity_log2 = 6) // 64 (§2.3)
        : cap_(1u << capacity_log2), mask_(cap_ - 1) {
        buf_ = new AudioCommand[cap_];
    }

    ~CommandQueue() { delete[] buf_; }

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    bool push(const AudioCommand& cmd) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= cap_) {
            return false;
        }
        buf_[size_t(head & mask_)] = cmd;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(AudioCommand& out) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf_[size_t(tail & mask_)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    AudioCommand* buf_ = nullptr;
    const uint32_t cap_;
    const uint32_t mask_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// Relaxed-atomic stats block the main thread reads (§2.3).
struct AudioStats {
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint32_t> late_events{0};
    std::atomic<uint32_t> dropped_events{0};
    std::atomic<uint32_t> stolen_voices{0};
    std::atomic<uint32_t> active_voices{0};
    std::atomic<uint32_t> peak_master_milli{0}; // |peak| * 1000
    std::atomic<uint32_t> callback_cpu_pct{0};
    std::atomic<uint64_t> acked_epoch{0};
};

struct AudioConfig {
    int master = 80; // 0..100 each; gain = (v/100)^2 (§3.1)
    int sfx = 100;
    int music = 60;
    int ui = 80;
    int period_frames = 0;     // 0 = ladder; else direct (128/256/512)
    bool null_backend = false; // --audio-null / no-device CI path
    bool latency_probe = false;
};

// The device + mixer + scheduler (12-audio.md §2-§4). One instance per
// process. All public methods are main/sim thread; the callback path
// itself (mix_locked core) is allocation-, lock- and exception-free.
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Device ladder 128->256->512 (§2.1) + the §2.2 startup log line.
    bool init(const AudioConfig& cfg);
    void shutdown();

    // ---- sim thread ----
    SoundEventQueue& sound_queue() { return *sounds_; }

    void publish_tick(uint64_t tick); // newest completed sim tick (§4.2)
    // §12 latency plumbing: {t0 of the input edge, its tick}. The
    // callback pairs it with the flipper voice start.
    void push_latency_probe(uint64_t t0_ns, uint32_t tick);

    // ---- main thread ----
    // Publishes a bank by atomic pointer + epoch; the previous bank is
    // reclaimed once the callback acks (pump() does the freeing).
    void publish_bank(std::unique_ptr<PatchBank> bank);
    bool push_command(const AudioCommand& cmd);
    void pump(); // each frame: reclaim retired banks

    const AudioStats& stats() const { return stats_; }

    // §12: p50/p95 of t_dac - t0 over captured flips (0 when empty).
    float measured_latency_p50_ms() const;
    float measured_latency_p95_ms() const;
    uint32_t measured_latency_n() const;

    int period_frames() const { return period_; }

    float estimated_latency_ms() const;

    // Offline render (demo artifact + tests): runs the exact callback
    // core over `frames` into interleaved stereo f32 without a device.
    void render_offline(float* out, uint32_t frames);

    // Test seams for the clock/scheduler math (null backend).
    double debug_spt() const;
    uint64_t debug_stream_pos() const;
    // Last kDebugStarts voice starts: {patch, absolute start sample}.
    // Exact scheduling evidence for the ±1 ms CI gate (12 §12).
    static constexpr uint32_t kDebugStarts = 32;

    struct DebugStart {
        uint16_t patch;
        uint16_t _pad;
        uint64_t sample;
    };

    uint32_t debug_starts(uint32_t count, DebugStart* out) const;

private:
    static void dataCallback(ma_device* device, void* out, const void* input, unsigned int frames);
    // The whole callback core; also the offline path.
    void mix(float* out, uint32_t frames);

    // Latency percentiles over the §12 probe buffer (0 when empty).
    float percentile_ms(int pct) const;

    AudioEngineImpl* impl_;

    // Cached for the main thread.
    AudioStats stats_;
    int period_ = 0;
    uint32_t periods_ = 2;
    bool null_backend_ = false;
    std::atomic<uint64_t> latest_tick_{0};
    SoundEventQueue* sounds_ = nullptr; // owned by impl_ (placement order)
};

} // namespace tb::audio
