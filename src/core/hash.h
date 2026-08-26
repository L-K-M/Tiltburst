#pragma once

#include <cstdint>
#include <string>

// FNV-1a 64-bit hash (16-testing-ci.md §2.1 known answer; §2.4.1 state
// serialization).
namespace tb {

inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime = 0x100000001b3ull;

constexpr uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t fnv1a64(const void* data, size_t len) {
    return fnv1a64(data, len, kFnvOffset);
}

constexpr uint64_t fnv1a64_str(const char* s, uint64_t seed = kFnvOffset) {
    uint64_t h = seed;
    while (*s) {
        h ^= static_cast<unsigned char>(*s++);
        h *= kFnvPrime;
    }
    return h;
}

} // namespace tb
