#include "support/alloc_hook.h"

#include <cstdlib>
#include <new>

namespace tb::test {
std::atomic<uint64_t> AllocCounter::news{0};
std::atomic<uint64_t> AllocCounter::deletes{0};
bool AllocCounter::enabled = false;
} // namespace tb::test

void* operator new(std::size_t size) {
    if (tb::test::AllocCounter::enabled) {
        tb::test::AllocCounter::news.fetch_add(1);
    }
    void* p = std::malloc(size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (tb::test::AllocCounter::enabled) {
        tb::test::AllocCounter::news.fetch_add(1);
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

void operator delete(void* ptr) noexcept {
    if (ptr != nullptr && tb::test::AllocCounter::enabled) {
        tb::test::AllocCounter::deletes.fetch_add(1);
    }
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    ::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    ::operator delete(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    ::operator delete(ptr);
}
