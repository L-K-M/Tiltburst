#include "render/art_renderer.h"

#include "core/log.h"

#include <cmath>

namespace tb::render {

namespace {

constexpr float kPiF = 3.14159265358979323846f;

// Premultiplied linear conversion of a packed 0xRRGGBBAA sRGB color.
// (The art pipeline authoring colors are sRGB; SdfInstance fill is
// premultiplied LINEAR per the sdf shader contract.)
void srgba_to_linear_premul(uint32_t c, float out[4]) {
    auto srgb_to_linear = [](float v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    const float r = srgb_to_linear(float((c >> 24) & 0xFFu) / 255.0f);
    const float g = srgb_to_linear(float((c >> 16) & 0xFFu) / 255.0f);
    const float b = srgb_to_linear(float((c >> 8) & 0xFFu) / 255.0f);
    const float a = float((c >> 0) & 0xFFu) / 255.0f;
    out[0] = r * a;
    out[1] = g * a;
    out[2] = b * a;
    out[3] = a;
}

float luminance_linear(const float rgba[4]) {
    // Relative luminance of the straight (non-premultiplied) color.
    const float a = rgba[3] > 0.0f ? rgba[3] : 1.0f;
    return 0.2126f * (rgba[0] / a) + 0.7152f * (rgba[1] / a) + 0.0722f * (rgba[2] / a);
}

} // namespace

void ArtRenderer::emit_prim(const ArtPrim& prim,
                            bool additive,
                            const sim::LightState* lights,
                            size_t light_count,
                            float sim_time_s,
                            std::vector<SdfInstance>& out) {
    if (out.size() >= kMaxInstances) {
        if (!warned_budget_) {
            warned_budget_ = true;
            TB_LOG_WARN("render", "art instance budget exceeded; truncating");
        }
        return;
    }

    // Light brightness multiplier (§3.2/§14.3): unlit = 15% fill, 0
    // glow; lit = 1.0 (patterns are the light layer's concern upstream
    // — the LightState.on flag carries the live state here; full
    // pattern timing lives in the sim's blink machinery).
    float light_mul = 1.0f;
    if (prim.light_index >= 0) {
        const bool on =
            size_t(prim.light_index) < light_count && lights[size_t(prim.light_index)].on;
        light_mul = on ? 1.0f : 0.15f;
    }

    // Fill (stop 0/1).
    float fill0[4] = {0, 0, 0, 0};
    float fill1[4] = {0, 0, 0, 0};
    srgba_to_linear_premul(prim.fill.color0, fill0);
    srgba_to_linear_premul(prim.fill.color1, fill1);
    if (prim.light_index >= 0) {
        // Unlit: keep 15% of the fill alpha (visible ghost floor).
        const float m = light_mul;
        for (int i = 0; i < 4; ++i) {
            fill0[i] *= m;
            fill1[i] *= m;
        }
    }

    // Glow (§2.3: default color = fill lightened 0.35 toward white in
    // linear space).
    float glow_rgb[3] = {0, 0, 0};
    float glow_intensity = 0.0f;
    if (prim.glow.radius > 0.0f && prim.glow.intensity > 0.0f) {
        glow_intensity = prim.glow.intensity * (prim.light_index >= 0 ? light_mul : 1.0f);
        if (prim.glow.has_color) {
            float g[4];
            srgba_to_linear_premul(prim.glow.color, g);
            glow_rgb[0] = g[0];
            glow_rgb[1] = g[1];
            glow_rgb[2] = g[2];
        } else {
            // Lerp fill straight-color toward white 0.35 (linear).
            const float straight0[4] = {fill0[0] / (fill0[3] > 0 ? fill0[3] : 1.0f),
                                        fill0[1] / (fill0[3] > 0 ? fill0[3] : 1.0f),
                                        fill0[2] / (fill0[3] > 0 ? fill0[3] : 1.0f),
                                        1.0f};
            for (int i = 0; i < 3; ++i) {
                glow_rgb[i] = straight0[i] + (1.0f - straight0[i]) * 0.35f;
            }
        }
    }

    // Stroke (premultiplied).
    float stroke_rgba[4] = {0, 0, 0, 0};
    if (prim.stroke.width > 0.0f) {
        srgba_to_linear_premul(prim.stroke.color, stroke_rgba);
        if (prim.light_index >= 0) {
            for (int i = 0; i < 4; ++i) {
                stroke_rgba[i] *= light_mul;
            }
        }
    }

    // Transform the local geometry to world (table meters).
    const float rad = prim.transform.rot_deg * kPiF / 180.0f;
    const float cs = std::cos(rad), sn = std::sin(rad);
    const float sc = prim.transform.scale;
    const float tx = prim.transform.pos[0], ty = prim.transform.pos[1];
    auto map_pt = [&](float x, float y, float& ox, float& oy) {
        ox = ((x * cs - y * sn) * sc) + tx;
        oy = ((x * sn + y * cs) * sc) + ty;
    };

    SdfInstance inst{};
    // Common paint.
    inst.rot = 0.0f;
    for (int i = 0; i < 4; ++i) {
        inst.fill0[i] = fill0[i];
        inst.fill1[i] = fill1[i];
    }
    // Gradient fields: xy dir, z len|radius, w mode.
    if (prim.fill.kind == Fill::Kind::Linear) {
        // Authored angle is LOCAL; rotate with the prim transform.
        const float a = (prim.fill.angle_deg + prim.transform.rot_deg) * kPiF / 180.0f;
        inst.grad[0] = std::cos(a);
        inst.grad[1] = std::sin(a);
        inst.grad[2] = prim.fill.length * sc;
        inst.grad[3] = 1.0f;
    } else if (prim.fill.kind == Fill::Kind::Radial) {
        inst.grad[0] = 0.0f;
        inst.grad[1] = 0.0f;
        inst.grad[2] = prim.fill.length * sc;
        inst.grad[3] = 2.0f;
    }
    inst.stroke[0] = stroke_rgba[0];
    inst.stroke[1] = stroke_rgba[1];
    inst.stroke[2] = stroke_rgba[2];
    inst.stroke[3] = prim.stroke.width * sc;
    inst.glow[0] = glow_rgb[0];
    inst.glow[1] = glow_rgb[1];
    inst.glow[2] = glow_rgb[2];
    inst.glow[3] = glow_intensity;

    switch (prim.kind) {
    case ArtPrim::Kind::Circle: {
        inst.kind = kSdfCircle;
        inst.cx = tx;
        inst.cy = ty;
        const float r = prim.r * sc;
        const float pad = prim.glow.radius * sc;
        inst.hx = inst.hy = r + pad;
        inst.p0 = r;
        break;
    }
    case ArtPrim::Kind::Ring: {
        inst.kind = kSdfRing;
        inst.cx = tx;
        inst.cy = ty;
        const float r = prim.r * sc;
        const float pad = prim.glow.radius * sc;
        inst.hx = inst.hy = r + prim.thickness * 0.5f * sc + pad;
        inst.p0 = r;
        inst.p1 = prim.thickness * 0.5f * sc;
        break;
    }
    case ArtPrim::Kind::Rect: {
        inst.kind = kSdfRbox;
        inst.cx = tx;
        inst.cy = ty;
        inst.rot = rad;
        const float hw = prim.w * 0.5f * sc, hh = prim.h * 0.5f * sc;
        const float pad = prim.glow.radius * sc;
        inst.hx = hw + pad;
        inst.hy = hh + pad;
        inst.p0 = prim.corner_r * sc;
        break;
    }
    case ArtPrim::Kind::Capsule: {
        inst.kind = kSdfCapsule;
        map_pt(prim.a[0], prim.a[1], inst.cx, inst.cy);
        float bx, by;
        map_pt(prim.b[0], prim.b[1], bx, by);
        const float r = prim.r * sc;
        const float pad = prim.glow.radius * sc;
        inst.cx = (inst.cx + bx) * 0.5f;
        inst.cy = (inst.cy + by) * 0.5f;
        const float dx = bx - inst.cx, dy = by - inst.cy;
        const float half_len = std::sqrt(dx * dx + dy * dy);
        inst.rot = std::atan2(dy, dx);
        inst.hx = half_len + r + pad;
        inst.hy = r + pad;
        inst.p0 = r;
        break;
    }
    case ArtPrim::Kind::Segment: {
        // Stroke-only: emit as a thin capsule.
        if (prim.stroke.width <= 0.0f) {
            return; // §3.3: stroke required
        }
        inst.kind = kSdfCapsule;
        float ax, ay, bx, by;
        map_pt(prim.a[0], prim.a[1], ax, ay);
        map_pt(prim.b[0], prim.b[1], bx, by);
        const float r = prim.stroke.width * 0.5f * sc;
        inst.cx = (ax + bx) * 0.5f;
        inst.cy = (ay + by) * 0.5f;
        const float dx = bx - inst.cx, dy = by - inst.cy;
        inst.rot = std::atan2(dy, dx);
        inst.hx = std::sqrt(dx * dx + dy * dy) + r;
        inst.hy = r;
        inst.p0 = r;
        // The stroke IS the fill here.
        for (int i = 0; i < 4; ++i) {
            inst.fill0[i] = stroke_rgba[i];
            inst.fill1[i] = stroke_rgba[i];
        }
        inst.stroke[3] = 0.0f;
        break;
    }
    case ArtPrim::Kind::Arc: {
        inst.kind = kSdfArc;
        inst.cx = tx;
        inst.cy = ty;
        // The SDF arc shader takes ABSOLUTE start/end; rot stays 0
        // (cycle-1: the start angle was applied twice).
        inst.rot = 0.0f;
        const float r = prim.r * sc;
        const float pad = prim.glow.radius * sc;
        inst.hx = inst.hy = r + prim.thickness * 0.5f * sc + pad;
        inst.p0 = r;
        inst.p1 = prim.thickness * 0.5f * sc;
        inst.p2 = prim.start_deg * kPiF / 180.0f;
        inst.p3 = prim.end_deg * kPiF / 180.0f; // absolute; rot == 0 for
                                                // unrotated arcs
        break;
    }
    case ArtPrim::Kind::Polyline: {
        // §3.3: lowered to capsules per segment with round joins —
        // each segment carries the polyline's stroke.
        if (prim.points.size() < 4) {
            return;
        }
        for (size_t i = 0; i + 3 < prim.points.size() && out.size() < kMaxInstances; i += 2) {
            SdfInstance seg = inst;
            seg.kind = kSdfCapsule;
            float ax, ay, bx, by;
            map_pt(prim.points[i], prim.points[i + 1], ax, ay);
            map_pt(prim.points[i + 2], prim.points[i + 3], bx, by);
            const float r = prim.stroke.width * 0.5f * sc;
            seg.cx = (ax + bx) * 0.5f;
            seg.cy = (ay + by) * 0.5f;
            const float dx = bx - seg.cx, dy = by - seg.cy;
            seg.rot = std::atan2(dy, dx);
            seg.hx = std::sqrt(dx * dx + dy * dy) + r;
            seg.hy = r;
            seg.p0 = r;
            for (int k = 0; k < 4; ++k) {
                seg.fill0[k] = stroke_rgba[k];
                seg.fill1[k] = stroke_rgba[k];
            }
            seg.stroke[3] = 0.0f;
            out.push_back(seg);
        }
        return; // already pushed per-segment
    }
    case ArtPrim::Kind::Polygon: {
        // §8.6 ear-clipped mesh path is a GPU milestone; v1 renders
        // the polygon as its stroke lowered to capsules (matches the
        // neon-outline style: shapes read by their glowing edges).
        // Closed polygons emit the closing edge (cycle-1 review); the
        // stroke IS the fill — inst's fill/gradient must not leak.
        const size_t n_pts = prim.points.size() / 2;
        const size_t n_segs = prim.closed ? n_pts : n_pts - 1;
        for (size_t s = 0; s < n_segs && out.size() < kMaxInstances; ++s) {
            SdfInstance seg = inst;
            seg.kind = kSdfCapsule;
            float ax, ay, bx, by;
            map_pt(prim.points[s * 2], prim.points[s * 2 + 1], ax, ay);
            const size_t j = ((s + 1) % n_pts) * 2;
            map_pt(prim.points[j], prim.points[j + 1], bx, by);
            const float r = std::max(prim.stroke.width * 0.5f, 0.0008f) * sc;
            seg.cx = (ax + bx) * 0.5f;
            seg.cy = (ay + by) * 0.5f;
            const float dx = bx - seg.cx, dy = by - seg.cy;
            seg.rot = std::atan2(dy, dx);
            seg.hx = std::sqrt(dx * dx + dy * dy) + r;
            seg.hy = r;
            seg.p0 = r;
            for (int k = 0; k < 4; ++k) {
                seg.fill0[k] = stroke_rgba[k];
                seg.fill1[k] = stroke_rgba[k];
            }
            seg.stroke[3] = 0.0f;
            seg.grad[3] = 0.0f; // solid
            out.push_back(seg);
        }
        return;
    }
    case ArtPrim::Kind::Text: {
        // Text instances are built by the font path at draw time
        // (sprite pipeline, not SDF); the art renderer marks them via
        // a degenerate circle that the text layer replaces. For M13a
        // v1 the placeholder is a capsule underline so layouts are
        // visible in smoke tests.
        inst.kind = kSdfCapsule;
        inst.cx = tx;
        inst.cy = ty - prim.size * 0.5f * sc;
        inst.hx = prim.size * sc;
        inst.hy = prim.size * 0.05f * sc;
        inst.p0 = inst.hy;
        inst.rot = 0.0f;
        break;
    }
    case ArtPrim::Kind::DecalGroup: {
        // Children were composed in WORLD space at load (their
        // transform is identity); recurse without re-transforming.
        for (const ArtPrim& child : prim.children) {
            if (out.size() >= kMaxInstances) {
                break;
            }
            // child.transform.pos is already world; emit directly.
            SdfInstance ci = inst;
            emit_prim(child, additive, lights, light_count, sim_time_s, out);
        }
        return;
    }
    default:
        return;
    }
    out.push_back(inst);
}

bool ArtRenderer::build(const sim::LightState* lights, size_t light_count, float sim_time_s) {
    below_.clear();
    above_.clear();
    if (art_ == nullptr) {
        return true;
    }
    for (const ArtLayer& layer : art_->layers) {
        std::vector<SdfInstance>& target = layer.z < 100 ? below_ : above_;
        for (const ArtPrim& prim : layer.prims) {
            emit_prim(prim, layer.additive, lights, light_count, sim_time_s, target);
        }
    }
    return below_.size() + above_.size() < kMaxInstances;
}

} // namespace tb::render
