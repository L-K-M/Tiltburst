p = "src/sim/solver.cpp"
s = open(p, encoding="utf-8").read()

# static call site → resolve_surface with zero Vs, no live catch
old_call = """                resolve_static(s, ball, best.normal,
                               material_row(best.collider->material));"""
new_call = """                resolve_surface(s, ball, best.normal,
                                material_row(best.collider->material),
                                {0.0f, 0.0f}, 1.0f);"""
assert old_call in s
s = s.replace(old_call, new_call)

# add resolve_flipper implementation after resolve_pair block end — anchor:
# "void Solver::find_earliest"
anchor = "Solver::Contact Solver::find_earliest"
flipper_impl = """void Solver::resolve_flipper(SimState& s, Ball& ball, Flipper& f,
                             const FlipperHit& hit) {
    const Material mat = material_row(MaterialId::FlipperRubber);

    // Live catch damping (§5.4): just-raised HOLD absorbs the ball.
    float scale = 1.0f;
    if (f.state == FlipperState::Hold &&
        f.ticks_since_eos <= kLiveCatchWindowTicks) {
        scale = kLiveCatchFactor;
    }

    resolve_surface(s, ball, hit.normal, mat, hit.surface_vel, scale);
}

"""
assert anchor in s
s = s.replace(anchor, flipper_impl + anchor)

# step 2: flipper state update before forces. Anchor on the forces comment.
old_forces = "    // Step 3 \u2014 forces + velocity update (FREE balls, index order)."
new_step2 = """    // Step 2 \u2014 flipper state update (\u00a75.2), id order; (theta_start,
    // omega) held constant for CCD this tick.
    for (Flipper& f : s.flippers) {
        bool pressed = f.enabled && f.button_latched;
        if (input != nullptr) {
            pressed = f.enabled && ((input->buttons >> f.params.action) & 1u) != 0u;
        }
        FlipperSim::tick(f, pressed);
    }

    // Step 3 \u2014 forces + velocity update (FREE balls, index order)."""
assert old_forces in s
s = s.replace(old_forces, new_step2)
open(p, "w", encoding="utf-8").write(s)
print("ok")
