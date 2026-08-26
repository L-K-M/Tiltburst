#include "platform/input.h"

#include "platform/input_internal.h"
#include "platform/latency.h"

#include <algorithm>
#include <string_view>

namespace tb::input {

std::atomic<uint32_t> g_button_bits{0};
std::atomic<bool> g_app_focused{true};
std::atomic<bool> g_raw_source_active{false};

void EdgeRing::push(const InputEdge& edge) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= kCapacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed); // §9.2 rule 1 alarm
        return;
    }
    slots_[head % kCapacity] = edge;
    head_.store(head + 1, std::memory_order_release);
}

size_t EdgeRing::pop(InputEdge* out, size_t max) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    const size_t available = static_cast<size_t>(std::min<uint64_t>(head - tail, max));
    for (size_t i = 0; i < available; ++i) {
        out[i] = slots_[(tail + i) % kCapacity];
    }
    if (available > 0) {
        tail_.store(tail + available, std::memory_order_release);
    }
    return available;
}

void Keymap::bind(int scancode, uint16_t action) {
    if (scancode < 0 || scancode >= static_cast<int>(kScancodeCount) || action >= kActionCount) {
        return;
    }
    table_[static_cast<size_t>(scancode)] |= (1u << action);
}

uint32_t Keymap::actions_for(int scancode) const {
    if (scancode < 0 || scancode >= static_cast<int>(kScancodeCount)) {
        return 0;
    }
    return table_[static_cast<size_t>(scancode)];
}

namespace {
const Keymap* g_keymap = nullptr; // swapped atomically on remap (§9.3)
} // namespace

void set_active_keymap(const Keymap* map) {
    g_keymap = map;
}

const Keymap* active_keymap() {
    return g_keymap;
}

uint32_t latch_input(InputSource** sources,
                     size_t count,
                     InputState& state,
                     uint64_t now_ns,
                     LatencyHistogram* latency_sink) {
    // §9.2 rule 2: at most 64 edges per tick across all rings.
    constexpr size_t kMaxEdgesPerTick = 64;

    // Drain in priority order into one merged buffer (each ring is FIFO;
    // cross-source order is deterministic by source priority).
    InputEdge merged[kMaxEdgesPerTick];
    size_t n_merged = 0;

    for (size_t s = 0; s < count && n_merged < kMaxEdgesPerTick; ++s) {
        InputSource* src = sources[s];
        if (src == nullptr || !src->active()) {
            continue;
        }

        InputEdge local[kMaxEdgesPerTick];
        const size_t want = std::min(kMaxEdgesPerTick, kMaxEdgesPerTick - n_merged);
        const size_t n = src->poll_edges(local, want);
        for (size_t i = 0; i < n; ++i) {
            merged[n_merged++] = local[i];
        }
    }

    const bool focused = g_app_focused.load(std::memory_order_relaxed);

    for (size_t i = 0; i < n_merged; ++i) {
        const InputEdge& e = merged[i];
        if (e.action >= kActionCount) {
            continue;
        }
        const bool gameplay = ((kGameplayActionMask >> e.action) & 1u) != 0u;
        // §9.9 focus gate: unfocused latches discard gameplay edges and
        // hold gameplay buttons released.
        if (!focused && gameplay) {
            continue;
        }
        const uint32_t bit = 1u << e.action;
        if (e.pressed != 0) {
            state.buttons |= bit;
            state.last_press_ns[e.action] = e.ts_ns;
            if (latency_sink != nullptr) {
                latency_sink->record(now_ns > e.ts_ns ? now_ns - e.ts_ns : 0);
            }
        } else {
            state.buttons &= ~bit;
            state.last_release_ns[e.action] = e.ts_ns;
        }
    }

    // §9.2 rule 4: reconcile level vs atomic bitset via synthetic edges so
    // the level and edge views never diverge (missed-edge resync, focus
    // regain). While unfocused, gameplay bits are held released (§9.9).
    uint32_t bits = g_button_bits.load(std::memory_order_acquire);
    if (!focused) {
        bits &= ~kGameplayActionMask;
    }
    for (uint16_t a = 0; a < kActionCount; ++a) {
        const uint32_t bit = 1u << a;
        const bool level = (state.buttons & bit) != 0;
        const bool atomic_level = (bits & bit) != 0;
        if (level != atomic_level) {
            if (atomic_level) {
                state.buttons |= bit;
                state.last_press_ns[a] = now_ns;
            } else {
                state.buttons &= ~bit;
                state.last_release_ns[a] = now_ns;
            }
        }
    }

    return state.buttons;
}

} // namespace tb::input
