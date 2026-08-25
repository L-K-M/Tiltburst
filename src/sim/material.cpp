#include "sim/types.h"

namespace tb::sim {

Material material_row(MaterialId id) {
    switch (id) {
    case MaterialId::Steel:
        return {0.45f, 0.15f, 0.10f, 0.50f};
    case MaterialId::Rubber:
        return {0.75f, 0.60f, 0.45f, 0.90f};
    case MaterialId::Plastic:
        return {0.35f, 0.20f, 0.12f, 0.40f};
    case MaterialId::FlipperRubber:
        return {0.85f, 0.60f, 0.45f, 0.90f};
    case MaterialId::Wood:
    default:
        return {0.30f, 0.25f, 0.15f, 0.60f};
    }
}

} // namespace tb::sim
