#pragma once

#include "audio/audio_bank.h"
#include "audio/sfx_synth.h"

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
// purpose map resolves at bank build (§6.2: "none" disables).
struct TableAudio {
    // JSON key order preserved (ids 24+ follow insertion order, §5.5).
    std::vector<std::pair<std::string, SfxPatch>> patches;
    // wav id -> pack-relative file path (decoded at bank build).
    std::vector<std::pair<std::string, std::string>> wav;
    // purpose key -> patch id string, or "none".
    std::map<std::string, std::string> map;
    bool has_songs = false; // songs are parsed for shape and deferred to M14
};

// Parses one patch object (§5.1 keys/ranges) — public for the
// assets-mirror sync test. Throws AudioLoadError.
SfxPatch parse_patch_json(const nlohmann::ordered_json& obj, const std::string& pointer);

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
