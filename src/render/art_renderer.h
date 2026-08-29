#pragma once

#include "render/sdf_batch.h"
#include "render/tbart.h"
#include "sim/solver.h"

#include <cstdint>
#include <vector>

namespace tb::render {

// Builds SdfInstances from a loaded TbArt document each frame
// (04-milestones.md M13a task 2). Pure CPU: the SdfBatch consumes the
// instances; light-bound primitives multiply their fill/glow by the
// live light brightness (unlit floor: 15% fill, 0 glow — §3.2/§14.3).
class ArtRenderer {
public:
    // Instance budget: art below the ball plus art above; the SDF batch
    // draws one instanced call per layer-pair (below/above ball).
    static constexpr size_t kMaxInstances = 8192;

    void set_art(const TbArt* art) { art_ = art; }

    const TbArt* art() const { return art_; }

    // Builds the below-ball instances (layers z < 100) and above-ball
    // (z >= 100) separately; the ball renders between them. `lights`
    // are the live LightState rows the art's light_index refers to.
    // Returns false when the instance budget overflowed (art is
    // truncated with a one-time log; the frame still renders).
    bool build(const sim::LightState* lights, size_t light_count, float sim_time_s);

    const std::vector<SdfInstance>& below_ball() const { return below_; }

    const std::vector<SdfInstance>& above_ball() const { return above_; }

private:
    void emit_prim(const ArtPrim& prim,
                   bool additive,
                   const sim::LightState* lights,
                   size_t light_count,
                   float sim_time_s,
                   std::vector<SdfInstance>& out);

    const TbArt* art_ = nullptr;
    std::vector<SdfInstance> below_;
    std::vector<SdfInstance> above_;
    bool warned_budget_ = false;
};

} // namespace tb::render
