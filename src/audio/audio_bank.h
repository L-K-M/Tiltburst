#pragma once

#include "audio/sfx_synth.h"
#include "sim/sound_out.h"

#include <cstdint>
#include <utility>
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
    // Intrusive retire-chain link (audio_engine's epoch protocol);
    // null unless this bank is retired awaiting an ack.
    PatchBank* retire_next = nullptr;
    // -1 when unknown ("none" is never an id — it disables a purpose).
    int find(const std::string& name) const;

    // §5.5: the bank is immutable after build — reads only.
    const std::vector<PatchEntry>& patch_entries() const { return entries_; }

    size_t size() const { return entries_.size(); }

    const PatchEntry& operator[](size_t i) const { return entries_[i]; }

    // Renders the 24 built-ins (§7.1, ids 0-23 in listed order).
    static std::unique_ptr<PatchBank> built_ins();
    // The compiled §7.1 parameter table (name, params) in id order —
    // the sync source for the assets/patches.json mirror test.
    static const std::vector<std::pair<std::string, SfxPatch>>& built_in_params();

    // Build-time mutation (built_ins and audio_json's build_bank);
    // closed after build — the §5.5 immutability the epoch protocol
    // relies on.
    std::vector<PatchEntry>& mutable_entries() { return entries_; }

    std::unordered_map<std::string, uint16_t>& mutable_names() { return by_name_; }

private:
    // Interned id -> entry. Ids 0..23 are the built-ins in §7.1 order;
    // table patches/wavs continue at 24 in JSON key order, and a table
    // patch with a built-in's name OVERRIDES it in place (same id).
    std::vector<PatchEntry> entries_;
    std::unordered_map<std::string, uint16_t> by_name_;
};

// The §7.2 purpose vocabulary lives in sim/sound_out.h (the emission
// side owns it — JOURNAL M11 layering decision); audio maps purpose
// indices to patch ids.
using sim::SoundPurpose;

// The 19 legal `map` keys (§7.2) in SoundPurpose order.
const char* purpose_key(SoundPurpose p);
// -1 when not a legal key (tb_validate error path).
int purpose_from_key(const std::string& key);

} // namespace tb::audio
