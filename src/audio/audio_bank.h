#pragma once

#include "audio/sfx_synth.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::audio {

// One playable sound: pre-rendered mono PCM at 48 kHz plus the mixer
// metadata (12-audio.md §3.1/§5.5). Banks are immutable after build and
// published to the audio thread by pointer (§2.3).
struct PatchEntry {
    std::string name;
    std::vector<float> pcm;
    uint8_t priority = 5; // voice stealing (§3.2)
    float gain = 1.0f;    // db_to_amp(volume_db), applied at render
};

class PatchBank {
public:
    // Interned id -> entry. Ids 0..23 are the built-ins in §7.1 order;
    // table patches/wavs continue at 24 in JSON key order, and a table
    // patch with a built-in's name OVERRIDES it in place (same id).
    std::vector<PatchEntry> entries;
    std::unordered_map<std::string, uint16_t> by_name;

    // -1 when unknown ("none" is never an id — it disables a purpose).
    int find(const std::string& name) const;

    // Renders the 24 built-ins (§7.1, ids 0-23 in listed order).
    static std::unique_ptr<PatchBank> built_ins();
};

// The default purpose map (§7.2): purpose key -> built-in patch id.
// Index by SoundPurpose enum.
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

// The 19 legal `map` keys (§7.2) in SoundPurpose order.
const char* purpose_key(SoundPurpose p);
// -1 when not a legal key (tb_validate error path).
int purpose_from_key(const std::string& key);

} // namespace tb::audio
