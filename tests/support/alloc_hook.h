#pragma once

#include <atomic>
#include <cstdint>
#include <new>

// Global allocation counter for the hot-path test (03-process.md §1.6:
// Solver::step must not allocate). Overriding operator new/delete in the
// test binary routes every heap allocation through these counters; the
// test enables counting around a scoped window only.
namespace tb::test {

struct AllocCounter {
    static std::atomic<uint64_t> news;
    static std::atomic<uint64_t> deletes;
    static bool enabled;
};

struct ScopedAllocCount {
    ScopedAllocCount() {
        AllocCounter::news.store(0);
        AllocCounter::deletes.store(0);
        AllocCounter::enabled = true;
    }

    ~ScopedAllocCount() { AllocCounter::enabled = false; }

    uint64_t news_total() const { return AllocCounter::news.load(); }

    uint64_t deletes_total() const { return AllocCounter::deletes.load(); }
};

} // namespace tb::test

// Replaced in tests/support/alloc_hook.cpp.
void* operator new(std::size_t size);
void* operator new[](std::size_t size);
void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept;
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, std::size_t) noexcept;
void operator delete[](void* ptr, std::size_t) noexcept;
void operator delete(void* ptr, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, const std::nothrow_t&) noexcept;
