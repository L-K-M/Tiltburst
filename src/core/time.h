#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// One timebase everywhere (05-engine-core.md §3, as amended by ADR-019):
// monotonic nanoseconds over the OS steady clock. Callable from any thread.
// The sim consumes time only as tick counts; pacing loops may call this.
uint64_t tb_now_ns();

namespace tb {

inline uint64_t now_ns() {
    return ::tb_now_ns();
}

// Spin-wait hint for the sleep-then-spin pacing shape (05 §6.2).
inline void cpu_pause() noexcept {
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __builtin_arm_yield();
#else
    asm volatile("");
#endif
}

constexpr uint64_t kSpinMarginNs = 300'000; // 300 µs (05 §6.2)

// Sleep until the absolute target, then spin the remainder: OS sleeps
// oversleep by ~55 µs–1 ms, so never sleep through the last margin.
void sleep_until_ns(uint64_t target_ns);

} // namespace tb
