#pragma once

// Internal seam shared by the input backend .cpp files: a source base with
// the §9.2 edge ring and canon §5.4 atomic latest-state already wired.
// Not part of the public input.h API.
#include "platform/input.h"

namespace tb::input {

class RingSource : public InputSource {
public:
    void submit(const InputEdge& edge) override {
        ring_.push(edge);
        const uint32_t bit = 1u << edge.action;
        if (edge.pressed != 0) {
            g_button_bits.fetch_or(bit, std::memory_order_release);
        } else {
            g_button_bits.fetch_and(~bit, std::memory_order_release);
        }
    }

    size_t poll_edges(InputEdge* out, size_t max) override { return ring_.pop(out, max); }

protected:
    EdgeRing ring_;
};

} // namespace tb::input
