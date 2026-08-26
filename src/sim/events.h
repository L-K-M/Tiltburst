#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Sim events and the SPSC overwrite ring (05-engine-core.md §8).
namespace tb::sim {

// Physics-side event types; values are stable (they enter state hashes and
// replay records). Script/framework types join at M9/M10.
enum class SimEventType : uint16_t {
    Collision = 1,    // ball vs static collider, speed ≥ 0.25 m/s (08 §4.1)
    Drain = 2,        // ball entered an outhole region (08 §6.15)
    BallLaunched = 3, // plunger release with a ball in the zone (08 §6.16)
    BallServed = 4,   // trough served a ball onto the plunger (08 §6.15)
    None = 0xFFFF,
};

struct SimEvent {
    uint64_t tick = 0;
    uint16_t type = uint16_t(SimEventType::None);
    uint16_t element = 0xFFFF;
    float x = 0.0f;
    float y = 0.0f;
    float a = 0.0f; // per-type payload (speed for collisions)
    float b = 0.0f;
    uint32_t data = 0;
};

static_assert(sizeof(SimEvent) == 32);

// Drop-oldest overwrite ring with a torn-read guard (05 §8.3). The
// producer is the sim thread only; the consumer is one fixed thread.
template <typename T, uint32_t N>
class EventRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static constexpr uint32_t kMargin = 64;
    static_assert(N > kMargin,
                  "N must exceed kMargin so resync arithmetic "
                  "cannot underflow");

public:
    void push(uint64_t tick, const T& ev) {
        uint64_t w = write_.load(std::memory_order_relaxed);
        slots_[w & (N - 1)] = ev;
        write_.store(w + 1, std::memory_order_release);
        (void)tick;
    }

    size_t drain(T* out, size_t max) {
        uint64_t w = write_.load(std::memory_order_acquire);
        if (w - read_ > N - kMargin) {
            resync(w);
        }
        size_t n = 0;
        while (read_ < w && n < max) {
            out[n] = slots_[read_ & (N - 1)];
            if (write_.load(std::memory_order_acquire) - read_ >= N) {
                w = write_.load(std::memory_order_relaxed);
                resync(w); // discard torn copy
                continue;
            }
            ++read_;
            ++n;
        }
        return n;
    }

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    void resync(uint64_t w) {
        const uint64_t new_read = w - (N - kMargin);
        dropped_.fetch_add(new_read - read_, std::memory_order_relaxed);
        read_ = new_read;
    }

    T slots_[N]{};
    std::atomic<uint64_t> write_{0};
    uint64_t read_ = 0; // consumer-private
    std::atomic<uint64_t> dropped_{0};
};

} // namespace tb::sim
