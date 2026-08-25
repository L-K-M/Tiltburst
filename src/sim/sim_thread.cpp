#include "sim/sim_thread.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/time.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <pthread.h>
#include <time.h>
#endif

namespace tb {

namespace {

constexpr uint64_t kDtNs = 1'000'000;    // exactly 1 ms (canon §5.3)
constexpr int64_t kMaxCatchUpTicks = 50; // §6.1 overrun clamp

void raise_priority_and_timer() {
    // Best-effort; failures are logged at debug and ignored (05 §6).
#if defined(_WIN32)
    timeBeginPeriod(1);
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        TB_LOG_DEBUG("sim", "SetThreadPriority(TIME_CRITICAL) failed");
    }
#else
    struct sched_param param {};

    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        TB_LOG_DEBUG("sim", "SCHED_FIFO unavailable; running at default priority");
    }
#endif
}

void lower_priority_and_timer() {
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

// Default pacing primitive: sleep until target - margin, spin the tail
// (05 §6.2). Injectable so tests drive ticks from a fake clock.
void default_wait(uint64_t target_ns) {
    const uint64_t wake = target_ns > kSpinMarginNs ? target_ns - kSpinMarginNs : 0;
    while (tb_now_ns() < wake) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(wake - tb_now_ns()));
        cpu_pause();
    }
    while (tb_now_ns() < target_ns) {
        cpu_pause();
    }
}

} // namespace

SimThread::~SimThread() {
    request_stop();
    join();
}

void SimThread::set_clock(ClockFn clock) {
    clock_ = clock;
}

void SimThread::set_wait(WaitFn wait) {
    wait_ = wait;
}

void SimThread::start(std::function<void(uint64_t)> tick_fn) {
    TB_CHECK(!thread_.joinable(), "SimThread already started");
    tick_fn_ = std::move(tick_fn);
    clock_ = clock_ ? clock_ : &tb_now_ns;
    wait_ = wait_ ? wait_ : &default_wait;
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
}

void SimThread::request_stop() {
    stop_.store(true, std::memory_order_release);
}

void SimThread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SimThread::run() {
    raise_priority_and_timer();

    const ClockFn now = clock_;
    const WaitFn wait_until_target = wait_;

    uint64_t t0 = now();
    uint64_t tick = 0;
    uint64_t accum_ns = 0;
    uint64_t last_ns = t0;

    while (!stop_.load(std::memory_order_acquire)) {
        // Pacing: absolute schedule anchored ONCE at thread start
        // (05 §6); the accumulator absorbs all drift.
        const uint64_t target_ns = t0 + (tick + 1) * kDtNs;
        wait_until_target(target_ns);

        // Accumulator over the injected/real timebase.
        uint64_t now_ns_val = now();
        accum_ns += now_ns_val - last_ns;
        last_ns = now_ns_val;
        int64_t pending = int64_t(accum_ns / kDtNs);

        // Overrun policy: clamp the burst, drop the debt (§6.1).
        if (pending > kMaxCatchUpTicks) {
            const int64_t dropped = pending - kMaxCatchUpTicks;
            dropped_ticks_.fetch_add(uint64_t(dropped), std::memory_order_relaxed);
            overruns_.fetch_add(1, std::memory_order_relaxed);
            TB_LOG_WARN_RATELIMITED("sim", "sim overrun: dropped {} ticks", dropped);
            pending = kMaxCatchUpTicks;
            accum_ns = uint64_t(kMaxCatchUpTicks) * kDtNs;
        }

        for (int64_t i = 0; i < pending; ++i) {
            if (tick_fn_) {
                tick_fn_(tick);
            }
            accum_ns -= kDtNs;
            ++tick;
        }

        ticks_run_.fetch_add(uint64_t(pending), std::memory_order_relaxed);
    }

    lower_priority_and_timer();
}

} // namespace tb
