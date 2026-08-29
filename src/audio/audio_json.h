#pragma once

#include "audio/audio_bank.h"
#include "audio/sfx_synth.h"
#include "audio/tracker.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tb::audio {

struct AudioLoadError : std::runtime_error {
    std::string json_pointer;

    AudioLoadError(const std::string& what, std::string pointer)
        : std::runtime_error(what), json_pointer(std::move(pointer)) {}
};

// Parsed audio.json (12-audio.md §6). Patches carry author params; the
// purpose map resolves at bank build (§6.2: "none" disables). Songs
// stay raw JSON here and parse at bank build, where the merged
// built-ins + table patch map exists to resolve instrument ids (§8.3).
struct TableAudio {
    // JSON key order preserved (ids 24+ follow insertion order, §5.5).
    std::vector<std::pair<std::string, SfxPatch>> patches;
    // wav id -> pack-relative file path (decoded at bank build).
    std::vector<std::pair<std::string, std::string>> wav;
    // purpose key -> patch id string, or "none".
    std::map<std::string, std::string> map;
    // song id -> raw song object (§8.3), parsed by build_bank.
    std::vector<std::pair<std::string, nlohmann::ordered_json>> songs;
    bool has_songs = false;
};

// Parses one patch object (§5.1 keys/ranges) — public for the
// assets-mirror sync test. Throws AudioLoadError.
SfxPatch parse_patch_json(const nlohmann::ordered_json& obj, const std::string& pointer);

// Parses one song object (§8.3 schema) against the merged patch map
// (built-ins + table patches by name). Enforces every §8.2/§8.3 range
// plus the §8.1 wave-per-channel rule; inst is required on a channel's
// first note. Throws AudioLoadError rooted at `pointer`.
TrackerSong parse_song_json(const nlohmann::ordered_json& obj,
                            const std::map<std::string, SfxPatch>& patches,
                            const std::string& pointer);

// Parses <dir>/audio.json. Throws AudioLoadError on schema violations
// (§5.1 ranges, unknown keys, sustain==0 && decay==0, illegal map keys,
// a patch/wav named "none"). A missing file returns false (callers fall
// back to built-ins). `//` comments allowed (canon §5.5).
bool load_audio_json(const std::filesystem::path& dir, TableAudio& out);

// Builds the final bank from built-ins + the table's patches/wav
// (overrides by name keep their id, §5.5), plus the resolved purpose
// map: purpose -> patch id or -1 (disabled/"none"/absent defaults come
// from §7.2). Wav paths resolve against `dir`.
std::unique_ptr<PatchBank> build_bank(const TableAudio& audio,
                                      const std::filesystem::path& dir,
                                      int (&purpose_patch)[int(SoundPurpose::Count)]);

} // namespace tb::audio
