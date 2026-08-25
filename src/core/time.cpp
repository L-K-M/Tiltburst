#include "core/time.h"

#include <ctime>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

uint64_t tb_now_ns() {
#if defined(_WIN32)
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    // Integer math stays exact for any QPC frequency; a double multiply
    // loses low bits once the counter exceeds 2^53 ticks.
    const uint64_t f = uint64_t(freq.QuadPart);
    const uint64_t t = uint64_t(counter.QuadPart);
    return (t / f) * 1'000'000'000ull + (t % f) * 1'000'000'000ull / f;
#else
    struct timespec ts {};

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#endif
}

void tb::sleep_until_ns(uint64_t target_ns) {
    // Guard the margin subtraction: a near-startup target smaller than the
    // spin margin must not wrap into a ~forever sleep.
    if (target_ns <= kSpinMarginNs) {
        while (tb_now_ns() < target_ns) {
            cpu_pause();
        }
        return;
    }

#if defined(_WIN32)
    // 1 ms timer resolution on Windows; the sim thread raised it via
    // timeBeginPeriod(1) (05 §6). Sleep in coarse chunks, spin the tail.
    for (;;) {
        const uint64_t now = tb_now_ns();
        if (now >= target_ns - kSpinMarginNs) {
            break;
        }
        const uint64_t remaining = target_ns - kSpinMarginNs - now;
        ::Sleep(DWORD(remaining / 1'000'000ull));
    }
    while (tb_now_ns() < target_ns) {
        cpu_pause();
    }
#else
    for (;;) {
        const uint64_t now = tb_now_ns();
        if (now >= target_ns) {
            return;
        }
        const uint64_t remaining = target_ns - now;
        if (remaining <= kSpinMarginNs) {
            while (tb_now_ns() < target_ns) {
                cpu_pause();
            }
            return;
        }

        struct timespec req {};

        req.tv_sec = time_t((remaining - kSpinMarginNs) / 1'000'000'000ull);
        req.tv_nsec = long((remaining - kSpinMarginNs) % 1'000'000'000ull);
        nanosleep(&req, nullptr);
    }
#endif
}
