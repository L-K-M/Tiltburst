#pragma once

#include <cstdint>

// 2-D vector math (08-physics.md §3.1 conventions).
namespace tb::sim {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vec2 operator+(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}

inline Vec2 operator-(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.y - b.y};
}

inline Vec2 operator*(Vec2 a, float s) {
    return {a.x * s, a.y * s};
}

inline Vec2 operator*(float s, Vec2 a) {
    return a * s;
}

inline Vec2 operator-(Vec2 a) {
    return {-a.x, -a.y};
}

inline Vec2& operator+=(Vec2& a, Vec2 b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}

inline Vec2& operator-=(Vec2& a, Vec2 b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}

inline Vec2& operator*=(Vec2& a, float s) {
    a.x *= s;
    a.y *= s;
    return a;
}

inline float dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

inline float cross(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

inline Vec2 perp(Vec2 v) {
    return {-v.y, v.x};
} // 90° CCW

inline float length_sq(Vec2 v) {
    return dot(v, v);
}

inline float wrap_ccw(float a) {
    // Map any angle to [0, 2π).
    const float two_pi = 6.283185307179586f;
    float r = a;
    while (r < 0.0f) {
        r += two_pi;
    }
    while (r >= two_pi) {
        r -= two_pi;
    }
    return r;
}

float length(Vec2 v);
Vec2 normalize(Vec2 v); // safe: returns (0,0) for zero vector

} // namespace tb::sim
