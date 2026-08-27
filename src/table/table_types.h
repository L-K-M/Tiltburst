#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// table.json schema types (09-table-format.md §2–§5), mirrored 1:1 for the
// M5 element set: wall, post, flipper, plunger, outhole, trough, light,
// plus the types the M5 prefabs and test-lab expand to (gate, rollover,
// pop_bumper, standup_target). Types arriving in M6–M8 extend the Element
// variant there. The load phase may throw (03-process.md §1.6).
namespace tb::table {

// §3 geometry path node: point or arc.
struct ArcNode {
    float to[2]{};
    float radius = 0.0f;
    bool cw = true; // bend direction as seen in table space
};

struct PathNode {
    bool is_arc = false;
    float point[2]{}; // when !is_arc
    ArcNode arc{};    // when is_arc
};

enum class MaterialId : uint8_t { Wood = 0, Steel, Rubber, Plastic };

struct WallDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    std::vector<PathNode> path;
    bool closed = false;
    MaterialId material = MaterialId::Wood;
};

struct PostDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{};
    float radius = 0.008f;
    MaterialId material = MaterialId::Rubber;
};

struct FlipperDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{};
    float length = 0.076f;
    float radius_base = 0.011f;
    float radius_tip = 0.007f;
    float rest_angle_deg = 0.0f;
    float swing_deg = 52.0f;
    bool left_side = true;
    std::string input; // "left" | "right" | "upper_left" | "upper_right"
    float strength = 1.0f;
};

struct PlungerDef {
    std::string id;
    std::vector<std::string> tags;
    float pos[2]{};
    float launch_angle_deg = 90.0f;
    float max_speed = 7.5f;
    float charge_time_s = 1.5f;
    bool auto_launch = false;
    float auto_delay_ms = 500.0f;
};

struct OutholeDef {
    std::string id;
    std::vector<std::string> tags;
    float a[2]{};
    float b[2]{};
};

struct TroughDef {
    std::string id;
    std::vector<std::string> tags;
    int capacity = 4;
};

struct LightDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{};
    std::string shape = "circle"; // circle | arrow | ring
    float size = 0.012f;
    std::string color;
    float direction_deg = 90.0f;
};

// M6 types the M5 prefabs and test-lab reference: parsed and retained, no
// physics yet (their element sims land in M6 per 04-milestones.md §M5).
struct GateDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    float width = 0.040f;
    float facing_deg = 90.0f;
    // "one_way" (default, mechanical) | "open" | "closed" (§6.7).
    bool state_open = false;
    bool state_closed = false;
};

struct HeightKey {
    float s = 0.0f; // normalized arc length 0..1
    float z = 0.0f; // meters above layer 0
};

struct RampDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    std::vector<PathNode> path;
    float width = 0.044f;
    std::vector<HeightKey> height_profile;
    bool drop_exit = false;
};

struct MagnetDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    float radius = 0.09f;
    float strength = 1.2f;
    bool default_on = false;
};

struct KickerDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    float radius = 0.014f;
    std::string style = "saucer"; // saucer | scoop | vuk
    float capture_ms = 800.0f;
    float eject_speed = 3.0f;
    float eject_angle_deg = 90.0f;
};

struct DropTargetBankDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;

    struct TargetDef {
        float pos[2]{};
        float width = 0.025f;
        float facing_deg = 0.0f;
    };

    std::vector<TargetDef> targets;
    bool auto_reset = false;
    float auto_reset_ms = 1500.0f;
};

struct CaptiveBallDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float a[2]{};
    float b[2]{};
};

struct BallLockDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    int capacity = 2;
    float eject_speed = 2.5f;
    float eject_angle_deg = -90.0f;
};

struct SpinnerDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    float facing_deg = 90.0f;
};

struct RolloverDef {
    std::string id;
    std::vector<std::string> tags;
    int layer = 0;
    float pos[2]{};
    float facing_deg = 90.0f;
};

struct SlingshotDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float face_a[2]{};
    float face_b[2]{};
    float kick_speed = 3.5f;
    float cooldown_ms = 80.0f;
};

struct PopBumperDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{};
    float radius = 0.031f;
    float kick_speed = 4.5f;
    float cooldown_ms = 60.0f;
};

struct StandupTargetDef {
    std::string id;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{};
    float width = 0.025f;
    float facing_deg = 0.0f;
    float min_speed = 0.3f;
};

struct Element {
    std::variant<WallDef,
                 PostDef,
                 FlipperDef,
                 PlungerDef,
                 OutholeDef,
                 TroughDef,
                 LightDef,
                 GateDef,
                 RolloverDef,
                 SpinnerDef,
                 RampDef,
                 MagnetDef,
                 KickerDef,
                 DropTargetBankDef,
                 CaptiveBallDef,
                 BallLockDef,
                 SlingshotDef,
                 PopBumperDef,
                 StandupTargetDef>
        def;

    const std::string& id() const;
    const std::vector<std::string>& tagsOf() const;
    const char* type_name() const;
};

struct PrefabInstance {
    std::string id;
    std::string prefab;
    int layer = 0;
    std::vector<std::string> tags;
    float pos[2]{}; // required by the M5 prefabs
    // flipper_pair_standard
    float tip_gap = 0.068f;
    float length = 0.076f;
    float rest_slope_deg = 31.0f;
    float swing_deg = 52.0f;
    float strength = 1.0f;
    // plunger_lane
    float lane_width = 0.040f;
    float top_y = 0.880f;
    bool auto_launch = false;
    float max_speed = 7.5f;
    float charge_time_s = 1.5f;
    // inlane_outlane_pair
    bool right_side = false;
    float mirror_axis_x = 0.240f;
    // orbit
    float mouth_x = 0.075f;
    float top_radius = 0.130f;
    float entry_y_left = 0.550f;
    float entry_y_right = 0.900f;
    // Loader plumbing (not schema): this lane merges with an orbit (§5.2
    // merged variant — no top post).
    bool merged_with_orbit = false;
    // sling_pair (§5.3)
    float sling_spread = 0.150f;
    float sling_face_length = 0.070f;
    float sling_tilt_deg = 22.0f;
    float sling_kick_speed = 3.5f;
};

struct PhysicsOverrides {
    bool present = false;
    float rolling_resistance = 0.025f;
    float restitution_falloff = 0.12f;
    float restitution_soft = 0.5f;
    float live_catch_window_ms = 50.0f;
    float live_catch_factor = 0.15f;
};

struct MaterialOverride {
    bool present = false;
    float restitution = -1.0f;
    float mu_s = -1.0f;
    float mu_k = -1.0f;
    float spin_transfer = -1.0f;
};

struct TableDef {
    // meta
    std::string slug;
    std::string name;
    std::string theme;
    std::string author;
    std::string description;
    std::string rules_card;

    // playfield
    float width = 0.52f;
    float height = 1.04f;
    float slope_deg = 6.5f;
    int ball_count = 4;
    float layer1_z = 0.055f;

    PhysicsOverrides physics;
    MaterialOverride materials[4]; // indexed by MaterialId order

    // Expanded elements (prefabs already applied, in document order).
    std::vector<Element> elements;
};

} // namespace tb::table
