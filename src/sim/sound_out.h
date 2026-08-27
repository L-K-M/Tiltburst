#pragma once

#include <cstdint>

// The sim's sound-emission port (12-audio.md §4.1/§7.2). The sim owns
// the payload and the purpose vocabulary; the audio system (tb_audio)
// consumes them — canon §5.1 keeps tb_sim linkable against tb_core
// only, so the dependency arrow points audio -> sim, never the reverse.
namespace tb::sim {

// Exactly 16 bytes (12-audio.md §4.1).
struct SoundEvent {
    uint32_t tick = 0;  // sim tick (1000 Hz) the sound occurs on
    uint16_t patch = 0; // interned patch id (resolved by the app)
    uint8_t flags = 0;  // bit0: duck; bits1-2: bus (0 sfx, 1 ui)
    uint8_t _pad = 0;
    float velocity = 1.0f; // 0..1, impact-derived at emission
    float pan = 0.0f;      // -1..+1, position-derived at emission
};

static_assert(sizeof(SoundEvent) == 16);

// Producer-side view of the sim->audio ring (the concrete queue lives
// in the app-owned AudioSystem). push returns false when full — the
// caller counts the drop (§4.1: drop the NEW event).
class SoundProducer {
public:
    virtual ~SoundProducer() = default;
    virtual bool push(const SoundEvent& ev) = 0;
};

// Intern table view for tb.play_sound name resolution (§5.5): a flat
// {name, id} list owned by the app alongside the published bank.
struct PatchIntern {
    const char* name;
    uint16_t id;
};

// The §7.2 automatic purposes. Index order is binding: SimState's
// purpose->patch array and audio_bank's default map both use it.
enum class SoundPurpose : uint8_t {
    Flipper = 0,
    Slingshot,
    PopBumper,
    StandupTarget,
    DropTarget,
    Spinner,
    Rollover,
    RampMade,
    Magnet,
    Kicker,
    Launch,
    Drain,
    TiltWarning,
    Tilt,
    BallLock,
    WallHit,
    BallBall,
    MenuMove,
    MenuSelect,
    Count
};

} // namespace tb::sim
