#pragma once

#include "sim/solver.h"
#include "table/table_types.h"

// TableDef → SimState (04-milestones.md M5). All pools allocate here; the
// sim hot path stays allocation-free afterwards (03-process.md §1.6).
namespace tb::table {

// Builds colliders (walls, posts, plunger face), flippers, plunger,
// outholes, trough, and lights into `out`, applying physics and material
// overrides. Element indices (collider.element_id, event element ids) are
// positions in def.elements.
void build_sim(const TableDef& def, tb::sim::SimState& out);

} // namespace tb::table
