#include "sim/flipper.h"

#include <cmath>

namespace tb::sim {

void FlipperSim::tick(Flipper& f, bool pressed) {
    const float swing = f.params.swing_rad();
    const int sign = f.params.side_sign;
    const float omega_max = kOmegaMax * f.params.strength;
    if (!f.enabled) {
        pressed = false; // disabled behaves as button-always-released
    }

    f.theta_start = f.theta;

    switch (f.state) {
    case FlipperState::Rest:
        f.progress = 0.0f;
        f.omega = 0.0f;
        f.ticks_since_eos = 0xFFFFFFFFu;
        if (pressed) {
            f.state = FlipperState::Rising;
            f.rise_ticks = 0;
        }
        break;

    case FlipperState::Rising: {
        // Linear ω ramp (constant α_rise), then cruise at ω_max.
        const float ramp = std::min(
            1.0f, float(f.rise_ticks * 0.001f) / kTauRise);
        f.omega = sign * omega_max * ramp;
        f.rise_ticks++;
        f.progress += std::fabs(f.omega) * kTickDt / swing;
        if (f.progress >= 1.0f) {
            f.progress = 1.0f;
            f.omega = 0.0f;
            f.state = FlipperState::Hold;
            f.ticks_since_eos = 0;
        } else if (!pressed) {
            f.state = FlipperState::Dropping;
            f.drop_omega = 0.0f;
        }
        break;
    }

    case FlipperState::Hold:
        f.progress = 1.0f;
        f.omega = 0.0f;
        f.ticks_since_eos++;
        if (!pressed) {
            f.state = FlipperState::Dropping;
            f.drop_omega = 0.0f;
            f.ticks_since_eos = 0xFFFFFFFFu;
        }
        break;

    case FlipperState::Dropping: {
        // Constant α toward rest, capped at ω_drop_max; no end bounce.
        f.drop_omega += kAlphaDrop * kTickDt;
        f.drop_omega = std::min(f.drop_omega, kOmegaDropMax);
        f.omega = -sign * f.drop_omega;
        f.progress -= f.drop_omega * kTickDt / swing;
        if (f.progress <= 0.0f) {
            f.progress = 0.0f;
            f.omega = 0.0f;
            f.state = FlipperState::Rest;
        } else if (pressed) {
            // Resume the rise from current |ω| if it already exceeds ramp.
            f.state = FlipperState::Rising;
            f.rise_ticks = uint32_t(std::fabs(f.omega) / omega_max *
                                    (kTauRise / kTickDt));
        }
        break;
    }
    }

    // θ advances after CCD used this tick's (θ_start, ω).
    f.theta = f.params.rest_rad() + sign * swing * f.progress;
    f.button_latched = pressed;
}

} // namespace tb::sim
