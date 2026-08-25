#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

// The 1000 Hz fixed-timestep sim thread (05-engine-core.md §6). Pacing is
// an absolute schedule (t0 + (tick+1) * dt), sleep-then-spin with a 300 µs
// margin; the accumulator clamps catch-up bursts at 50 ticks and drops the
// rest with a ratelimited warn (§6.1). dt = 0.001 s exactly (canon §5.3).
namespace tb {

class SimThread {
public:
    // Test seams: clock returns monotonic ns; wait sleeps until target.
    using ClockFn = uint64_t (*)();
    using WaitFn = void (*)(uint64_t target_ns);

    SimThread() = default;
    ~SimThread();

    void set_clock(ClockFn clock); // before start(); default tb_now_ns
    void set_wait(WaitFn wait);    // before start(); default sleep_until_ns

    void start(std::function<void(uint64_t tick)> tick_fn);
    void request_stop();
    void join();

    // Counters (05 §6.1); readable from any thread.
    uint64_t ticks_run() const { return ticks_run_.load(std::memory_order_relaxed); }

    uint32_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

    uint64_t dropped_ticks() const { return dropped_ticks_.load(std::memory_order_relaxed); }

private:
    void run();

    std::function<void(uint64_t)> tick_fn_;
    ClockFn clock_ = nullptr;
    WaitFn wait_ = nullptr;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> ticks_run_{0};
    std::atomic<uint32_t> overruns_{0};
    std::atomic<uint64_t> dropped_ticks_{0};
};

} // namespace tb
