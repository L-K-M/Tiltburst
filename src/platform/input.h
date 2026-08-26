#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Raw input system (05-engine-core.md §9): fixed actions, edge queues,
// producers, and the late-latch contract. Headers here stay SDL-free;
// platform backends live in the .cpp files.
namespace tb::input {

// §9.1 fixed action indices — never renumber (record format §13, settings
// §11).
enum Action : uint16_t {
    kActionLeftFlipper = 0,
    kActionRightFlipper = 1,
    kActionLeftFlipper2 = 2,
    kActionRightFlipper2 = 3,
    kActionPlunger = 4,
    kActionNudgeLeft = 5,
    kActionNudgeRight = 6,
    kActionNudgeUp = 7,
    kActionStart = 8,
    kActionPause = 9,
    kActionUiFirst = 10, // ui_up..ui_back are SDL-only, not remappable
    kActionCount = 16,   // InputState array width
};

// Edge sources (§9.1).
constexpr uint8_t kSourceSdl = 0;
constexpr uint8_t kSourceWinRaw = 1;
constexpr uint8_t kSourceEvdev = 2;
constexpr uint8_t kSourceSynthetic = 3;
constexpr uint8_t kSourceReplay = 4;

// Gameplay-class actions 0–8 are suppressed on the SDL source while a raw
// source is active (§9.8) and discarded while unfocused (§9.9). UI-class
// actions always come from SDL and are never gated.
constexpr uint32_t kGameplayActionMask = (1u << 9) - 1;

struct InputEdge {
    uint64_t ts_ns = 0;  // tb::now_ns() timebase, taken at the OS event
    uint16_t action = 0; // §9.1 action index
    uint8_t pressed = 0; // 1 press, 0 release
    uint8_t source = 0;  // 0 SDL, 1 WinRaw, 2 evdev, 3 synthetic, 4 replay
};

// Canon §5.4 atomic latest-state, updated by producers with fetch_or /
// fetch_and(release) on every edge.
extern std::atomic<uint32_t> g_button_bits;

// §9.9 main-thread focus gate (windowed mode); defaults to true so headless
// runs and tests never drop edges.
extern std::atomic<bool> g_app_focused;

// §9.8 raw-source activity flag: while true, the SDL source drops
// gameplay-class edges at the pump (they would arrive twice).
extern std::atomic<bool> g_raw_source_active;

// §9.2 edge queue: one ring per producer. SPSC — one producer thread, the
// sim thread consumes in latch_input(). Capacity 1024 per §9.2 rule 1; a
// full ring drops the new edge and raises `dropped` (a harness bug alarm).
class EdgeRing {
public:
    void push(const InputEdge& edge);
    // Drains up to `max` edges in FIFO order; returns how many were read.
    size_t pop(InputEdge* out, size_t max);

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t kCapacity = 1024; // power of two
    static_assert((kCapacity & (kCapacity - 1)) == 0);

    std::array<InputEdge, kCapacity> slots_{};
    std::atomic<uint64_t> head_{0}; // producer cursor (slot index)
    std::atomic<uint64_t> tail_{0}; // consumer cursor
    std::atomic<uint64_t> dropped_{0};
};

// Scancode-keyed action map shared by every producer (§9.3): entries are
// SDL_Scancode values; each maps to zero or more action indices. Built once
// from Settings.bindings at boot (rebuild-on-settings-change arrives with
// runtime remapping).
class Keymap {
public:
    static constexpr size_t kScancodeCount = 512;

    // Binds `scancode` (an SDL_Scancode value) to `action`.
    void bind(int scancode, uint16_t action);

    // Action bits for a scancode, as a bitmask over §9.1 indices.
    uint32_t actions_for(int scancode) const;

private:
    std::array<uint32_t, kScancodeCount> table_{};
};

// Builds the keymap from settings binding names via SDL_GetScancodeFromName
// (§9.3); unknown names log a warn and are skipped. Defined in input_sdl.cpp.
Keymap build_keymap_from_settings(const std::array<std::vector<std::string>, 10>& bindings);

// Per-tick level state (§9.1 InputState), rebuilt every tick by latch_input.
struct InputState {
    uint32_t buttons = 0; // bit i = action i held
    std::array<uint64_t, kActionCount> last_press_ns{};
    std::array<uint64_t, kActionCount> last_release_ns{};
};

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual bool start() = 0;             // spawn thread / register; false = unavailable
    virtual void stop() = 0;              // join/unregister; idempotent
    virtual const char* name() const = 0; // "sdl", "winraw", "evdev"
    // Sim thread calls this inside latch_input(); drains this source's ring.
    virtual size_t poll_edges(InputEdge* out, size_t max) = 0;

    // True while this source delivers gameplay actions (§9.8).
    virtual bool active() const { return true; }

    // Producer-side edge injection (used internally and by tests).
    virtual void submit(const InputEdge& edge) = 0;
};

// §14.1 cumulative input→latch histogram; declared fully in latency.h.
class LatencyHistogram;

// §9.8 factories. make_sdl_input_source is always available; the raw
// sources return nullptr off their platform.
std::unique_ptr<InputSource> make_sdl_input_source();
std::unique_ptr<InputSource> make_winraw_input_source();
std::unique_ptr<InputSource> make_evdev_input_source();

// §9.3: installs the keymap raw producers read lock-free. Call before
// start(); v1 builds the map once at boot.
void set_active_keymap(const Keymap* map);
const Keymap* active_keymap();

// §9.4 main-thread hook: translate one SDL key event into edges on the
// SDL source (defined in input_sdl.cpp; needs SDL at link time only).
void sdl_key_input(InputSource* source,
                   const Keymap& keymap,
                   int scancode,
                   bool down,
                   bool repeat,
                   uint64_t ts_ns);

// §9.2 rule 2/4/5 + §9.9: called once per tick immediately before the
// physics step. Drains at most 64 edges across all rings, applies them to
// `state`, reconciles against g_button_bits via synthetic edges, applies
// the focus gate to gameplay actions, and returns the TickInput button word
// for this tick. Press-edge latencies (now − ts_ns) feed the §14.1
// histogram through `latency_sink` when non-null.
uint32_t latch_input(InputSource** sources,
                     size_t count,
                     InputState& state,
                     uint64_t now_ns,
                     class LatencyHistogram* latency_sink);

} // namespace tb::input
