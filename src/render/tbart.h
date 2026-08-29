#pragma once

#include "sim/solver.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// TBArt — the art.json vector format (13-art-direction.md §3, canon §5.5).
// Loader + in-memory model; prefab expansion happens at load so the
// renderer only ever sees concrete primitives.
namespace tb::render {

// §2.2 — the five canon palettes (binding hex, sRGB) + resolved roles.
struct Palette {
    std::string name;
    std::array<uint32_t, 8> rgba; // bg0..glow_white, packed 0xRRGGBBAA
};

// Resolves a palette name ("sunset-synth"...) or nullptr for unknown.
const Palette* canon_palette(const std::string& name);
// Role index for a name; -1 unknown. Order: bg0,bg1,primary,secondary,
// accent1,accent2,warm,glow_white.
int palette_role(const std::string& role);

struct ArtError : std::runtime_error {
    std::string json_pointer;

    ArtError(const std::string& what, std::string pointer)
        : std::runtime_error(what), json_pointer(std::move(pointer)) {}
};

struct Fill {
    enum class Kind : uint8_t { Solid = 0, Linear, Radial };
    Kind kind = Kind::Solid;
    uint32_t color0 = 0xFFFFFFFF; // stop 0 (or the solid)
    uint32_t color1 = 0xFFFFFFFF; // stop 1
    float angle_deg = 90.0f;      // linear
    float length = 0.0f;          // linear / radius (radial)
};

struct Stroke {
    float width = 0.0f; // meters
    uint32_t color = 0xFFFFFFFF;
};

struct Glow {
    float radius = 0.0f;    // meters
    float intensity = 1.0f; // 0..2
    bool has_color = false; // §2.3 default: lerp(fill, white, 0.35)
    uint32_t color = 0xFFFFFFFF;
};

struct Transform {
    float pos[2] = {0.0f, 0.0f};
    float rot_deg = 0.0f;
    float scale = 1.0f;
};

// One concrete primitive post-expansion. Star→polygon, decal→children,
// polyline stays polyline (the renderer strokes it as capsules).
struct ArtPrim {
    enum class Kind : uint8_t {
        Circle = 0,
        Ring,
        Rect,
        Capsule,
        Segment,
        Polyline,
        Polygon,
        Arc,
        Text,
        DecalGroup, // a decal's expanded children share the transform
    };
    Kind kind = Kind::Circle;
    Transform transform;

    // Shape parameters (per-kind; meters, local space).
    float r = 0.0f;            // circle/ring/arc
    float thickness = 0.0f;    // ring/arc
    float w = 0.0f, h = 0.0f;  // rect
    float corner_r = 0.0f;     // rect
    float a[2] = {0.0f, 0.0f}; // capsule/segment endpoints
    float b[2] = {0.0f, 0.0f};
    std::vector<float> points; // polyline/polygon: x,y pairs
    bool closed = false;
    float start_deg = 0.0f, end_deg = 360.0f; // arc

    // Text (§3.3).
    std::string text;
    uint8_t font = 0; // 0 orbitron-role, 1 monoton, 2 righteous
    float size = 0.0f;
    uint8_t align = 0; // 0 left, 1 center, 2 right
    float letter_spacing = 0.0f;

    // Paint.
    Fill fill;
    Stroke stroke;
    Glow glow;

    // Light binding (§3.2): index into SimState lights by table_id is
    // resolved at load; -1 = unbound.
    int light_index = -1;

    // Decal children (DecalGroup only).
    std::vector<ArtPrim> children;
};

struct ArtLayer {
    std::string name;
    int z = 0;             // 0..199; <100 below ball (§3.1)
    bool additive = false; // "blend": normal|additive
    std::vector<ArtPrim> prims;
};

struct TbArt {
    std::string palette_name;
    Palette palette;
    bool ball_trail = false;
    uint32_t ball_trail_color = 0xFFFFFFFF;
    std::vector<ArtLayer> layers;
};

// Loads <dir>/art.json. A missing file returns false (tables render
// with the greybox); corrupt throws ArtError. Prefabs expand, stars
// become polygons, colors resolve to hex (roles resolved against the
// palette). `light_ids` maps table.json light ELEMENT IDS (strings)
// to indices into the art renderer's light table; a "light" field
// naming an unknown id is a load error (validated).
struct ArtLoadResult {
    bool loaded = false; // false: no art.json
    TbArt art;
};

ArtLoadResult load_art(const std::filesystem::path& dir,
                       const std::vector<std::pair<std::string, int>>& light_ids);

// Resolves "primary" / "#RRGGBB[AA]" against a palette; throws
// ArtError on anything else (validated: unknown color).
uint32_t resolve_color(const std::string& color, const Palette& palette);

} // namespace tb::render
