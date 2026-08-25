#pragma once

#include <cstdint>

// PCG32 — the only RNG in the sim (05-engine-core.md §10, verbatim).
// All sim randomness flows through this class; no rand, std::mt19937,
// SDL_rand, or math.random anywhere in tb_sim or Lua.
namespace tb {

class Pcg32 {
public:
    void seed(uint64_t initstate, uint64_t initseq) {
        state_ = 0u;
        inc_ = (initseq << 1u) | 1u; // stream id; must be odd
        next_u32();
        state_ += initstate;
        next_u32();
    }

    uint32_t next_u32() { // PCG-XSH-RR, O'Neill reference
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, 1). 24-bit mantissa, exact, never returns 1.0.
    float next_float() { return (next_u32() >> 8) * 0x1.0p-24f; }

    // Uniform in [0, bound), bound >= 1. Unbiased (threshold rejection).
    uint32_t next_below(uint32_t bound) {
        uint32_t threshold = (uint32_t)(-bound) % bound; // = 2^32 mod bound
        for (;;) {
            uint32_t r = next_u32();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    // Uniform integer in [lo, hi] inclusive, lo <= hi.
    int32_t range_i32(int32_t lo, int32_t hi) {
        return lo + (int32_t)next_below((uint32_t)(hi - lo) + 1u);
    }

    // Uniform float in [lo, hi).
    float range_f32(float lo, float hi) { return lo + (hi - lo) * next_float(); }

private:
    uint64_t state_ = 0x853c49e6748fea9bULL; // reference defaults
    uint64_t inc_ = 0xda3e39cb94b95bdbULL;
};

} // namespace tb
