#include "core/config.h"
#include "core/log.h"
#include "platform/input.h"
#include "platform/input_internal.h"

#include <SDL3/SDL.h>

namespace tb::input {

namespace {

// SDL producer: edges are pushed from the main-thread event pump (§9.4)
// and consumed by the sim thread inside latch_input().
class SdlInputSource final : public RingSource {
public:
    const char* name() const override { return "sdl"; }

    bool start() override { return true; }

    void stop() override {}
};

SdlInputSource* g_sdl_source = nullptr;

} // namespace

Keymap build_keymap_from_settings(const std::array<std::vector<std::string>, 10>& bindings) {
    Keymap map;
    for (size_t action = 0; action < bindings.size(); ++action) {
        for (const std::string& name : bindings[action]) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                TB_LOG_WARN("input", "unknown key name in bindings: {}", name);
                continue;
            }
            map.bind(static_cast<int>(sc), static_cast<uint16_t>(action));
        }
    }
    return map;
}

std::unique_ptr<InputSource> make_sdl_input_source() {
    auto src = std::make_unique<SdlInputSource>();
    g_sdl_source = src.get();
    return src;
}

// §9.4: one edge per mapped action per non-repeat key event; called from
// the main thread's SDL_PollEvent loop with the raw key fields. While a
// raw source is active (§9.8), gameplay-class edges are suppressed here —
// at the source — so they never ring and never touch the atomic.
void sdl_key_input(InputSource* source,
                   const Keymap& keymap,
                   int scancode,
                   bool down,
                   bool repeat,
                   uint64_t ts_ns) {
    if (source == nullptr || repeat) {
        return; // autorepeat emits nothing
    }
    const bool suppress_gameplay = g_raw_source_active.load(std::memory_order_acquire);
    const uint32_t actions = keymap.actions_for(scancode);
    for (uint16_t action = 0; action < kActionCount; ++action) {
        if (((actions >> action) & 1u) == 0u) {
            continue;
        }
        const bool gameplay = ((kGameplayActionMask >> action) & 1u) != 0u;
        if (suppress_gameplay && gameplay) {
            continue;
        }
        source->submit(InputEdge{ts_ns, action, down ? uint8_t(1) : uint8_t(0), kSourceSdl});
    }
}

} // namespace tb::input
