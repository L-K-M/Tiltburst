#include "audio/audio_json.h"

#include "core/log.h"
#include "miniaudio.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tb::audio {

using json = nlohmann::ordered_json; // §5.5: ids 24+ follow JSON key order

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& pointer) {
    throw AudioLoadError("audio.json: " + what, pointer);
}

float get_param(
    const json& obj, const char* key, float def, float lo, float hi, const std::string& pointer) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (!it->is_number()) {
        fail(std::string("'") + key + "' must be a number", pointer + "/" + key);
    }
    const double v = it->get<double>();
    if (!(v >= double(lo) && v <= double(hi))) {
        fail(std::string("'") + key + "' outside " + std::to_string(lo) + ".." + std::to_string(hi),
             pointer + "/" + key);
    }
    return float(v);
}

Wave get_wave(const json& obj, const std::string& pointer) {
    const auto it = obj.find("wave");
    if (it == obj.end()) {
        return Wave::Square;
    }
    if (!it->is_string()) {
        fail("'wave' must be a string", pointer + "/wave");
    }
    const std::string w = it->get<std::string>();
    if (w == "square")
        return Wave::Square;
    if (w == "saw")
        return Wave::Saw;
    if (w == "sine")
        return Wave::Sine;
    if (w == "noise")
        return Wave::Noise;
    if (w == "triangle")
        return Wave::Triangle;
    fail("unknown wave '" + w + "'", pointer + "/wave");
}

// The complete §5.1 key set; anything else is an error.
bool is_known_key(const std::string& k) {
    static const char* const kKeys[] = {
        "wave",
        "duty",
        "duty_sweep",
        "attack",
        "sustain",
        "punch",
        "decay",
        "base_freq",
        "freq_limit",
        "freq_slide",
        "freq_delta_slide",
        "vib_depth",
        "vib_speed",
        "arp_mod",
        "arp_speed",
        "repeat_speed",
        "flanger_offset",
        "flanger_sweep",
        "lpf_cutoff",
        "lpf_sweep",
        "lpf_resonance",
        "hpf_cutoff",
        "hpf_sweep",
        "volume_db",
        "priority",
    };
    for (const char* known : kKeys) {
        if (k == known) {
            return true;
        }
    }
    return false;
}

} // namespace

SfxPatch parse_patch_json(const json& obj, const std::string& pointer) {
    if (!obj.is_object()) {
        fail("patch must be an object", pointer);
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!is_known_key(it.key())) {
            fail("unknown patch key '" + it.key() + "'", pointer + "/" + it.key());
        }
    }
    SfxPatch p;
    p.wave = get_wave(obj, pointer);
    p.duty = get_param(obj, "duty", p.duty, 0.02f, 0.98f, pointer);
    p.duty_sweep = get_param(obj, "duty_sweep", p.duty_sweep, -1.0f, 1.0f, pointer);
    p.attack = get_param(obj, "attack", p.attack, 0.0f, 1.0f, pointer);
    p.sustain = get_param(obj, "sustain", p.sustain, 0.0f, 1.0f, pointer);
    p.punch = get_param(obj, "punch", p.punch, 0.0f, 1.0f, pointer);
    p.decay = get_param(obj, "decay", p.decay, 0.0f, 1.0f, pointer);
    p.base_freq = get_param(obj, "base_freq", p.base_freq, 0.0f, 1.0f, pointer);
    p.freq_limit = get_param(obj, "freq_limit", p.freq_limit, 0.0f, 1.0f, pointer);
    p.freq_slide = get_param(obj, "freq_slide", p.freq_slide, -1.0f, 1.0f, pointer);
    p.freq_delta_slide =
        get_param(obj, "freq_delta_slide", p.freq_delta_slide, -1.0f, 1.0f, pointer);
    p.vib_depth = get_param(obj, "vib_depth", p.vib_depth, 0.0f, 1.0f, pointer);
    p.vib_speed = get_param(obj, "vib_speed", p.vib_speed, 0.0f, 1.0f, pointer);
    p.arp_mod = get_param(obj, "arp_mod", p.arp_mod, -1.0f, 1.0f, pointer);
    p.arp_speed = get_param(obj, "arp_speed", p.arp_speed, 0.0f, 1.0f, pointer);
    p.repeat_speed = get_param(obj, "repeat_speed", p.repeat_speed, 0.0f, 1.0f, pointer);
    p.flanger_offset = get_param(obj, "flanger_offset", p.flanger_offset, -1.0f, 1.0f, pointer);
    p.flanger_sweep = get_param(obj, "flanger_sweep", p.flanger_sweep, -1.0f, 1.0f, pointer);
    p.lpf_cutoff = get_param(obj, "lpf_cutoff", p.lpf_cutoff, 0.0f, 1.0f, pointer);
    p.lpf_sweep = get_param(obj, "lpf_sweep", p.lpf_sweep, -1.0f, 1.0f, pointer);
    p.lpf_resonance = get_param(obj, "lpf_resonance", p.lpf_resonance, 0.0f, 1.0f, pointer);
    p.hpf_cutoff = get_param(obj, "hpf_cutoff", p.hpf_cutoff, 0.0f, 1.0f, pointer);
    p.hpf_sweep = get_param(obj, "hpf_sweep", p.hpf_sweep, -1.0f, 1.0f, pointer);
    p.volume_db = get_param(obj, "volume_db", p.volume_db, -24.0f, 6.0f, pointer);
    const float prio = get_param(obj, "priority", float(p.priority), 0.0f, 9.0f, pointer);
    p.priority = uint8_t(prio);
    if (p.sustain == 0.0f && p.decay == 0.0f) {
        fail("sustain == 0 && decay == 0 (the envelope never opens)", pointer);
    }
    return p;
}

// §5.5: a wav entry decodes to 48 kHz mono PCM via ma_decoder; stereo
// downmixes 0.5*(L+R); longer than 10 s is an error.
namespace {

bool decode_wav(const std::filesystem::path& dir,
                const std::string& rel,
                std::vector<float>& out_pcm) {
    // Validate ONLY the pack-relative string from table JSON (cycle-13
    // blocker: testing the JOINED path rejected every wav whenever the
    // table dir was absolute — always, on Windows). The pack dir itself
    // is trusted and may legitimately be absolute or carry a drive
    // letter. ':' also rejects drive-relative forms (C:foo).
    const std::filesystem::path rel_path(rel);
    // has_root_path() also rejects root-relative ("/x", "\x") and
    // drive/UNC forms that is_absolute() misses on Windows (an
    // absolute path always HAS a root path, so this subsumes it).
    // ".." is checked per COMPONENT: "foo..bar.wav" is a legal name,
    // only an actual parent traversal escapes.
    // Normalize separators FIRST: on Windows the path iterator only
    // splits on '\', so "foo/../bar" would be one component and the
    // hop check would miss it (cycle-19 review). The generic format
    // splits both separators.
    bool parent_hop = false;
    for (const auto& part : std::filesystem::path(rel_path.generic_string())) {
        if (part == "..") {
            parent_hop = true;
            break;
        }
    }
    if (rel_path.has_root_path() || parent_hop || rel.find(':') != std::string::npos) {
        TB_LOG_ERROR("audio", "wav path '{}' escapes the pack", rel);
        return false;
    }
    const std::filesystem::path path = dir / rel_path;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 48000);
    ma_decoder dec;
#if defined(_WIN32)
    // Narrow-char paths break on non-ASCII directories (cycle-13).
    if (ma_decoder_init_file_w(path.wstring().c_str(), &cfg, &dec) != MA_SUCCESS) {
#else
    if (ma_decoder_init_file(path.string().c_str(), &cfg, &dec) != MA_SUCCESS) {
#endif
        return false;
    }
    constexpr ma_uint64 kMaxFrames = 48000ull * 10;
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) != MA_SUCCESS) {
        ma_decoder_uninit(&dec);
        TB_LOG_ERROR("audio", "wav '{}' cannot report its length", path.string());
        return false;
    }
    if (frames > kMaxFrames) {
        ma_decoder_uninit(&dec);
        TB_LOG_ERROR("audio", "wav '{}' exceeds 10 s", path.string());
        return false;
    }
    out_pcm.resize(size_t(frames));
    ma_uint64 read = 0;
    if (!out_pcm.empty()) {
        ma_decoder_read_pcm_frames(&dec, out_pcm.data(), frames, &read);
    }
    ma_decoder_uninit(&dec);
    out_pcm.resize(size_t(read));
    return read > 0;
}

} // namespace

bool load_audio_json(const std::filesystem::path& dir, TableAudio& out) {
    const std::filesystem::path file = dir / "audio.json";
    std::error_code ec;
    const bool present = std::filesystem::exists(file, ec);
    if (ec || !present) {
        if (ec) {
            // A stat failure is not "missing": surface it rather than
            // silently shipping built-ins for an unreadable pack.
            fail("audio.json existence check failed: " + ec.message(), "/audio.json");
        }
        return false; // optional file: built-ins cover everything (§6)
    }
    std::ifstream in(file);
    if (!in.good()) {
        // Present but unreadable (permissions): a SILENT built-ins
        // fallback would hide author mistakes — fail loudly instead.
        fail("audio.json exists but cannot be read", "/audio.json");
    }
    json doc;
    try {
        doc = json::parse(in, nullptr, true, true); // comments (canon §5.5)
    } catch (const json::parse_error& e) {
        fail(std::string("parse error: ") + e.what(), "");
    }
    if (!doc.is_object()) {
        fail("root must be an object", "");
    }
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key() != "patches" && it.key() != "wav" && it.key() != "songs" &&
            it.key() != "map") {
            fail("unknown top-level key '" + it.key() + "'", "/" + it.key());
        }
    }

    if (doc.contains("patches")) {
        const json& ps = doc.at("patches");
        if (!ps.is_object()) {
            fail("'patches' must be an object", "/patches");
        }
        for (auto it = ps.begin(); it != ps.end(); ++it) {
            if (it.key() == "none") {
                fail("a patch may not be named \"none\" (reserved, §6.2c)", "/patches/" + it.key());
            }
            out.patches.emplace_back(it.key(), parse_patch_json(*it, "/patches/" + it.key()));
        }
    }
    if (doc.contains("wav")) {
        const json& wv = doc.at("wav");
        if (!wv.is_object()) {
            fail("'wav' must be an object", "/wav");
        }
        for (auto it = wv.begin(); it != wv.end(); ++it) {
            if (it.key() == "none") {
                fail("a wav may not be named \"none\" (reserved, §6.2c)", "/wav/" + it.key());
            }
            if (!it->is_string()) {
                fail("wav entry must be a path string", "/wav/" + it.key());
            }
            out.wav.emplace_back(it.key(), it->get<std::string>());
        }
    }
    if (doc.contains("songs")) {
        const json& sg = doc.at("songs");
        if (!sg.is_object()) {
            fail("'songs' must be an object", "/songs");
        }
        for (auto it = sg.begin(); it != sg.end(); ++it) {
            if (it.key().empty()) {
                fail("song id must be a non-empty string", "/songs");
            }
            out.songs.emplace_back(it.key(), *it);
        }
        out.has_songs = !out.songs.empty();
    }
    if (doc.contains("map")) {
        const json& mp = doc.at("map");
        if (!mp.is_object()) {
            fail("'map' must be an object", "/map");
        }
        for (auto it = mp.begin(); it != mp.end(); ++it) {
            if (purpose_from_key(it.key()) < 0) {
                fail("map key '" + it.key() +
                         "' is not a §7.2 purpose (script-played sounds go in rules.lua, "
                         "§6.2)",
                     "/map/" + it.key());
            }
            if (!it->is_string()) {
                fail("map value must be a patch id or \"none\"", "/map/" + it.key());
            }
            out.map.emplace(it.key(), it->get<std::string>());
        }
    }
    return true;
}

// Shared insert: override a same-named entry in place, else append —
// with the 0xFFFF id-space cap enforced in ONE place (cycle-9 review:
// the wav loop had drifted from the patches loop's guard).
void insert_into_bank(PatchBank& bank,
                      const std::string& name,
                      PatchEntry entry,
                      const std::string& pointer) {
    const auto existing = bank.mutable_names().find(name);
    if (existing != bank.mutable_names().end()) {
        bank.mutable_entries()[existing->second] = std::move(entry); // override in place
        return;
    }
    if (bank.mutable_entries().size() >= 0xFFFF) {
        fail("patch bank exceeds 65535 entries (id space)", pointer);
    }
    bank.mutable_names().emplace(name, uint16_t(bank.mutable_entries().size()));
    bank.mutable_entries().push_back(std::move(entry));
}

// ---- tracker songs (§8.2/§8.3) ----

namespace {

const char* const kSongChannelNames[TrackerPattern::kChannels] = {
    "pulse1", "pulse2", "wide", "noise"};

// §8.1 wave legality per channel.
bool wave_legal_for_channel(int ch, Wave w) {
    switch (ch) {
    case 0:
    case 1:
        return w == Wave::Square;
    case 2:
        return w == Wave::Saw || w == Wave::Triangle || w == Wave::Sine;
    case 3:
        return w == Wave::Noise;
    default:
        return false;
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "C-4" / "A#4" / "---" / "OFF" -> the TrackerCell note encoding.
uint8_t parse_note_token(const std::string& tok, const std::string& ptr) {
    if (tok == "---") {
        return 0;
    }
    if (tok == "OFF") {
        return TrackerCell::kOff;
    }
    if (tok.size() != 3 || (tok[1] != '-' && tok[1] != '#')) {
        fail("bad note token '" + tok + "' (want C-4, A#4, ---, OFF)", ptr);
    }
    // Semitone per letter: C..G in slots 0,2,4,5,7; A,B in 8,10.
    int sem = -1;
    switch (tok[0]) {
    case 'C':
        sem = 0;
        break;
    case 'D':
        sem = 2;
        break;
    case 'E':
        sem = 4;
        break;
    case 'F':
        sem = 5;
        break;
    case 'G':
        sem = 7;
        break;
    case 'A':
        sem = 9;
        break;
    case 'B':
        sem = 11;
        break;
    default:
        fail("bad note letter '" + tok + "'", ptr);
    }
    if (tok[1] == '#') {
        ++sem;
    }
    const int oct = tok[2] - '0';
    if (tok[2] < '0' || tok[2] > '8') {
        fail("bad octave in '" + tok + "' (0-8)", ptr);
    }
    const int midi = (oct + 1) * 12 + sem;
    if (midi < 12 || midi > 119) { // C-0 .. B-8 (§8.2)
        fail("note '" + tok + "' out of the C-0..B-8 range", ptr);
    }
    return uint8_t(midi - 11); // 1..108
}

int parse_int_token(const std::string& tok, int lo, int hi, const std::string& ptr) {
    int v = 0;
    if (tok.empty()) {
        fail("bad number '" + tok + "'", ptr);
    }
    for (char c : tok) {
        if (c < '0' || c > '9') {
            fail("bad number '" + tok + "'", ptr);
        }
        v = v * 10 + (c - '0');
        if (v > hi) {
            break; // clamp check below reports with the real value
        }
    }
    if (v < lo || v > hi) {
        fail("value '" + tok + "' out of " + std::to_string(lo) + "-" + std::to_string(hi), ptr);
    }
    return v;
}

// Parses the fx token (§8.3): A<hex>, S+n/S-n, V<d>,<s>.
void parse_fx_token(const std::string& tok, TrackerCell& cell, const std::string& ptr) {
    // +1 for the 'A' itself; longer tokens cannot fit the cell's arp.
    if (tok.size() >= 2 && tok[0] == 'A' && tok.size() <= 1 + TrackerCell::kArpCap) {
        cell.arp_n = uint8_t(tok.size() - 1);
        for (size_t i = 1; i < tok.size(); ++i) {
            const int d = hex_digit(tok[i]);
            if (d < 0) {
                fail("bad arp hex digit in '" + tok + "'", ptr);
            }
            cell.arp[i - 1] = uint8_t(d);
        }
        cell.fx = 1;
        return;
    }
    if (tok.size() >= 3 && tok[0] == 'S' && (tok[1] == '+' || tok[1] == '-')) {
        const int n = parse_int_token(tok.substr(2), 1, 12, ptr);
        cell.slide = int8_t(tok[1] == '-' ? -n : n);
        cell.fx = 2;
        return;
    }
    if (tok.size() >= 4 && tok[0] == 'V') {
        const size_t comma = tok.find(',');
        if (comma == std::string::npos || comma < 2 || comma + 1 >= tok.size()) {
            fail("bad vibrato '" + tok + "' (want V<d>,<s>)", ptr);
        }
        cell.vib_depth = uint8_t(parse_int_token(tok.substr(1, comma - 1), 0, 15, ptr));
        cell.vib_speed = uint8_t(parse_int_token(tok.substr(comma + 1), 0, 15, ptr));
        cell.fx = 3;
        return;
    }
    fail("bad fx token '" + tok + "' (A<hex>, S+n, S-n, V<d>,<s>)", ptr);
}

// Parses `note [inst] [vol] [fx]` (1-4 tokens, `.` skips a middle
// slot). `inst_name` receives the instrument token when present.
TrackerCell
parse_cell_text(const std::string& text, std::string& inst_name, const std::string& ptr) {
    TrackerCell cell;
    inst_name.clear();
    std::array<std::string, 4> tok;
    int n_tok = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(unsigned(text[i]))) {
            ++i;
        }
        size_t j = i;
        while (j < text.size() && !std::isspace(unsigned(text[j]))) {
            ++j;
        }
        if (j > i) {
            if (n_tok >= 4) {
                fail("cell has more than 4 tokens: '" + text + "'", ptr);
            }
            tok[size_t(n_tok++)] = text.substr(i, j - i);
        }
        i = j;
    }
    if (n_tok == 0) {
        return cell; // empty cell: nothing new this row
    }
    cell.note = parse_note_token(tok[0], ptr);
    int pos = 1;
    if (pos < n_tok) {
        if (tok[size_t(pos)] != ".") {
            inst_name = tok[size_t(pos)];
        }
        ++pos;
    }
    if (pos < n_tok) {
        if (tok[size_t(pos)] != ".") {
            cell.vol = uint8_t(parse_int_token(tok[size_t(pos)], 0, 15, ptr));
        }
        ++pos;
    }
    if (pos < n_tok) {
        if (tok[size_t(pos)] != ".") {
            parse_fx_token(tok[size_t(pos)], cell, ptr);
        }
        ++pos;
    }
    return cell;
}

} // namespace

TrackerSong parse_song_json(const nlohmann::ordered_json& obj,
                            const std::map<std::string, SfxPatch>& patches,
                            const std::string& base) {
    TrackerSong song;
    const auto p = [&base](const std::string& leaf) { return base + leaf; };

    if (!obj.is_object()) {
        fail("song must be an object", base);
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() != "bpm" && it.key() != "ticks_per_row" && it.key() != "patterns" &&
            it.key() != "order") {
            fail("unknown song key '" + it.key() + "'", p("/" + it.key()));
        }
    }
    if (!obj.contains("bpm") || !obj.contains("ticks_per_row") || !obj.contains("patterns") ||
        !obj.contains("order")) {
        fail("song needs bpm, ticks_per_row, patterns, order", base);
    }
    if (!obj.at("bpm").is_number()) {
        fail("bpm must be a number", p("/bpm"));
    }
    const double bpm = obj.at("bpm").get<double>();
    if (bpm < 40.0 || bpm > 260.0) {
        fail("bpm out of 40-260", p("/bpm"));
    }
    song.bpm = float(bpm);
    if (!obj.at("ticks_per_row").is_number_unsigned()) {
        fail("ticks_per_row must be an integer 1-31", p("/ticks_per_row"));
    }
    const uint64_t tpr = obj.at("ticks_per_row").get<uint64_t>();
    if (tpr < 1 || tpr > 31) {
        fail("ticks_per_row out of 1-31", p("/ticks_per_row"));
    }
    song.ticks_per_row = uint32_t(tpr);

    // Patterns: all four channels each, 16 or 32 rows, song-wide
    // consistent (§8.3).
    const json& pats = obj.at("patterns");
    if (!pats.is_object() || pats.empty()) {
        fail("'patterns' must be a non-empty object", p("/patterns"));
    }
    std::vector<
        std::pair<std::string, std::array<std::vector<std::string>, TrackerPattern::kChannels>>>
        raw;
    for (auto it = pats.begin(); it != pats.end(); ++it) {
        if (it.key().empty()) {
            fail("pattern name must be non-empty", p("/patterns"));
        }
        const std::string pptr = p("/patterns/" + it.key());
        if (!it->is_object()) {
            fail("pattern must be an object", pptr);
        }
        for (auto pk = it->begin(); pk != it->end(); ++pk) {
            bool known = false;
            for (const char* cn : kSongChannelNames) {
                known = known || pk.key() == cn;
            }
            if (!known) {
                fail("unknown pattern key '" + pk.key() +
                         "' (channels are pulse1/"
                         "pulse2/wide/noise, §8.3)",
                     pptr + "/" + pk.key());
            }
        }
        std::array<std::vector<std::string>, TrackerPattern::kChannels> chans;
        for (uint32_t c = 0; c < TrackerPattern::kChannels; ++c) {
            if (!it->contains(kSongChannelNames[c])) {
                fail(std::string("pattern is missing channel '") + kSongChannelNames[c] + "'",
                     pptr);
            }
            const json& arr = it->at(kSongChannelNames[c]);
            if (!arr.is_array() || arr.empty()) {
                fail(std::string("channel '") + kSongChannelNames[c] +
                         "' must be a non-empty array",
                     pptr + "/" + kSongChannelNames[c]);
            }
            std::vector<std::string> cells;
            cells.reserve(arr.size());
            for (size_t r = 0; r < arr.size(); ++r) {
                if (arr[r].is_null()) {
                    cells.emplace_back();
                } else if (arr[r].is_string()) {
                    cells.push_back(arr[r].get<std::string>());
                } else {
                    fail("cells must be strings (or null)",
                         pptr + "/" + kSongChannelNames[c] + "/" + std::to_string(r));
                }
            }
            if (cells.size() != 16 && cells.size() != 32) {
                fail("channel rows must be 16 or 32", pptr + "/" + kSongChannelNames[c]);
            }
            chans[c] = std::move(cells);
        }
        for (uint32_t c = 1; c < TrackerPattern::kChannels; ++c) {
            if (chans[c].size() != chans[0].size()) {
                fail("channels disagree on row count inside one pattern", pptr);
            }
        }
        if (!raw.empty() && chans[0].size() != raw.front().second[0].size()) {
            fail("all patterns in one song must share the row count (§8.3)", pptr);
        }
        raw.emplace_back(it.key(), std::move(chans));
    }
    const size_t rows = raw.front().second[0].size();

    // Order: 1..128 names, every one defined (§8.3).
    const json& ord = obj.at("order");
    if (!ord.is_array() || ord.empty() || ord.size() > 128) {
        fail("'order' must be an array of 1-128 pattern names", p("/order"));
    }
    for (size_t i = 0; i < ord.size(); ++i) {
        if (!ord[i].is_string()) {
            fail("order entries must be pattern-name strings", p("/order/" + std::to_string(i)));
        }
        song.order.push_back(ord[i].get<std::string>());
    }
    for (const std::string& name : song.order) {
        bool found = false;
        for (const auto& [pname, chans] : raw) {
            if (pname == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            fail("order names pattern '" + name + "' which is not defined", p("/order"));
        }
    }

    // Cells: parse text, collect instruments (dedup, order-preserving),
    // enforce inst-on-first-note and the §8.1 wave rule per channel.
    std::vector<std::string> instr_names;
    const auto intern_instr = [&instr_names, &base](const std::string& n) -> int {
        for (size_t i = 0; i < instr_names.size(); ++i) {
            if (instr_names[i] == n) {
                return int(i);
            }
        }
        if (instr_names.size() >= 0xFF) {
            // 0xFF is the cell's no-inst sentinel, so legal indices are
            // 0..254: the song exceeds 255 instruments.
            fail("song uses more than 255 distinct instruments", base);
        }
        instr_names.push_back(n);
        return int(instr_names.size() - 1);
    };
    // First pass: parse + instrument interning.
    std::vector<std::pair<std::string, TrackerPattern>> parsed;
    for (auto& [pname, chans] : raw) {
        TrackerPattern pat;
        pat.rows = uint32_t(rows);
        for (uint32_t c = 0; c < TrackerPattern::kChannels; ++c) {
            pat.chan[c].reserve(rows);
        }
        parsed.emplace_back(pname, std::move(pat));
    }
    // The sticky-inst rule is per channel across the WHOLE song and
    // runs in PLAYBACK order (§8.3): `order` decides which pattern's
    // note is the channel's first, not the patterns object's
    // declaration order (cycle-2 review). Patterns never referenced
    // by `order` still parse — each validated standalone.
    auto parse_pattern_cells =
        [&](size_t pi, bool(&has_inst)[TrackerPattern::kChannels], bool fresh) {
            auto& [pname, chans] = raw[pi];
            TrackerPattern& pat = parsed[pi].second;
            if (fresh) {
                for (uint32_t c = 0; c < TrackerPattern::kChannels; ++c) {
                    has_inst[c] = false;
                }
            }
            const std::string pptr = p("/patterns/" + pname);
            for (uint32_t c = 0; c < TrackerPattern::kChannels; ++c) {
                for (size_t r = 0; r < rows; ++r) {
                    const std::string cptr =
                        pptr + "/" + kSongChannelNames[c] + "/" + std::to_string(r);
                    std::string inst_name;
                    TrackerCell cell = parse_cell_text(chans[c][r], inst_name, cptr);
                    if (!inst_name.empty()) {
                        cell.inst = uint8_t(intern_instr(inst_name));
                    }
                    if (cell.note != 0 && cell.note != TrackerCell::kOff && !has_inst[c] &&
                        cell.inst == 0xFF) {
                        fail(std::string("channel '") + kSongChannelNames[c] +
                                 "' needs an inst on its first PLAYED note (§8.3)",
                             cptr);
                    }
                    if (cell.inst != 0xFF) {
                        has_inst[c] = true;
                    }
                    pat.chan[c].push_back(cell);
                }
            }
        };
    bool has_inst[TrackerPattern::kChannels] = {false, false, false, false};
    std::vector<bool> played(raw.size(), false);
    for (const std::string& name : song.order) {
        for (size_t pi = 0; pi < raw.size(); ++pi) {
            if (raw[pi].first == name) {
                if (!played[pi]) {
                    parse_pattern_cells(pi, has_inst, /*fresh=*/false);
                    played[pi] = true;
                }
                break;
            }
        }
    }
    for (size_t pi = 0; pi < raw.size(); ++pi) {
        if (!played[pi]) {
            parse_pattern_cells(pi, has_inst, /*fresh=*/true);
        }
    }
    // Resolve instruments through the merged patch map; enforce the
    // §8.1 wave rule at every use site (the same patch may be legal on
    // one channel and not another).
    for (const std::string& n : instr_names) {
        const auto it = patches.find(n);
        if (it == patches.end()) {
            fail("instrument '" + n +
                     "' does not resolve to a patch or built-in (wav ids cannot be "
                     "tracker instruments, §8.1)",
                 base);
        }
        const SfxPatch& sp = it->second;
        TrackerInstr ti;
        ti.wave = sp.wave;
        ti.duty = sp.duty;
        // §5.1-mapped seconds (x^2 * 2.268).
        ti.attack = sp.attack * sp.attack * 2.268f;
        ti.sustain = sp.sustain;
        ti.release = sp.decay * sp.decay * 2.268f;
        ti.gain = db_to_amp(sp.volume_db);
        song.instruments.push_back(ti);
    }
    for (auto& [pname, pat] : parsed) {
        for (uint32_t c = 0; c < TrackerPattern::kChannels; ++c) {
            for (TrackerCell& cell : pat.chan[c]) {
                if (cell.inst == 0xFF) {
                    continue;
                }
                const SfxPatch& sp = patches.at(instr_names[cell.inst]);
                if (!wave_legal_for_channel(int(c), sp.wave)) {
                    fail("instrument '" + instr_names[cell.inst] + "' (wave " +
                             std::to_string(int(sp.wave)) + ") is illegal on channel '" +
                             kSongChannelNames[c] + "' (§8.1)",
                         p("/patterns/" + pname));
                }
            }
        }
    }
    song.patterns = std::move(parsed);
    return song;
}

std::unique_ptr<PatchBank> build_bank(const TableAudio& audio,
                                      const std::filesystem::path& dir,
                                      int (&purpose_patch)[int(SoundPurpose::Count)]) {
    // Defaults from §7.2 (built-in ids follow the §7.1 listing order).
    struct DefaultMap {
        SoundPurpose purpose;
        const char* patch;
    };

    static const DefaultMap kDefaults[] = {
        {SoundPurpose::Flipper, "flipper_clack"},
        {SoundPurpose::Slingshot, "sling_thwack"},
        {SoundPurpose::PopBumper, "pop_bumper_ding"},
        {SoundPurpose::StandupTarget, "target_thud"},
        {SoundPurpose::DropTarget, "drop_target_clunk"},
        {SoundPurpose::Spinner, "spinner_tick"},
        {SoundPurpose::Rollover, "rollover_chime"},
        {SoundPurpose::RampMade, "ramp_whoosh"},
        {SoundPurpose::Magnet, "magnet_hum"},
        {SoundPurpose::Kicker, "kicker_pop"},
        {SoundPurpose::Launch, "launch_spring"},
        {SoundPurpose::Drain, "drain_womp"},
        {SoundPurpose::TiltWarning, "tilt_warning_buzz"},
        {SoundPurpose::Tilt, "tilt_alarm"},
        {SoundPurpose::BallLock, "lock_clunk"},
        {SoundPurpose::WallHit, "target_thud"},
        {SoundPurpose::BallBall, "lock_clunk"},
        {SoundPurpose::MenuMove, "menu_move"},
        {SoundPurpose::MenuSelect, "menu_select"},
    };

    auto bank = PatchBank::built_ins();

    // Table patches: override a built-in's name in place (same id),
    // otherwise append at 24+ in JSON key order (§5.5).
    for (const auto& [name, patch] : audio.patches) {
        PatchEntry e;
        e.name = name;
        e.priority = patch.priority;
        e.gain = 1.0f; // volume_db applied at render (§5.4)
        if (!render_patch(patch, name, e.pcm)) {
            fail("patch '" + name + "' rendered silence", "/patches/" + name);
        }
        insert_into_bank(*bank, name, std::move(e), "/patches/" + name);
    }
    for (const auto& [name, rel] : audio.wav) {
        PatchEntry e;
        e.name = name;
        e.priority = 5;
        e.gain = 1.0f;
        if (!decode_wav(dir, rel, e.pcm)) {
            fail("wav '" + name + "' could not be decoded from '" + rel + "'", "/wav/" + name);
        }
        insert_into_bank(*bank, name, std::move(e), "/wav/" + name);
    }

    for (const DefaultMap& d : kDefaults) {
        purpose_patch[int(d.purpose)] = bank->find(d.patch);
    }
    for (const auto& [key, target] : audio.map) {
        const int purpose = purpose_from_key(key);
        if (target == "none") {
            purpose_patch[purpose] = -1; // disabled (§6.2c)
            continue;
        }
        const int id = bank->find(target);
        if (id < 0) {
            fail("map value '" + target + "' does not resolve to a patch, wav, or built-in",
                 "/map/" + key);
        }
        purpose_patch[purpose] = id;
    }

    // ---- tracker songs (§8): instruments resolve against the merged
    // built-ins + table patch map (wavs are not synth patches and
    // cannot be tracker instruments — §8.1 names patch fields). ----
    std::map<std::string, SfxPatch> patch_map;
    for (const auto& [name, params] : PatchBank::built_in_params()) {
        patch_map.emplace(name, params);
    }
    for (const auto& [name, patch] : audio.patches) {
        patch_map[name] = patch; // override by name (§5.5)
    }
    for (const auto& [id, raw] : audio.songs) {
        bank->mutable_songs().push_back({id, parse_song_json(raw, patch_map, "/songs/" + id)});
    }

    // ---- duck triggers (§10): the fixed patch-name list. ----
    static const char* const kDuckTriggers[] = {
        "jackpot_hit", "multiball_riser", "extra_ball_fanfare", "tilt_alarm", "drain_womp"};
    for (const char* name : kDuckTriggers) {
        const int id = bank->find(name);
        if (id >= 0 && bank->duck_patch_n < PatchBank::kDuckPatchCap) {
            bank->duck_patch[bank->duck_patch_n++] = uint16_t(id);
        }
    }
    return bank;
}

} // namespace tb::audio
