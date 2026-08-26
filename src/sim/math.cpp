#include "sim/math.h"

#include <cmath>

namespace tb::sim {

float length(Vec2 v) {
    return std::sqrt(length_sq(v));
}

Vec2 normalize(Vec2 v) {
    const float len = length(v);
    if (!(len > 0.0f)) { // rejects zero AND NaN
        return {0.0f, 0.0f};
    }
    return {v.x / len, v.y / len};
}

} // namespace tb::sim
