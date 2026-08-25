#include "sim/replay.h"

#include <cstdio>
#include <cstring>

namespace tb::sim {

namespace {

constexpr char kMagic[4] = {'T', 'B', 'R', 'P'};

#pragma pack(push, 1)

struct FileHeader {
    char magic[4];
    uint16_t version;
    uint16_t pad;
    uint64_t seed;
    char table[64];
    uint64_t tick_count;
    uint8_t nudge_level;
    uint8_t reserved[7];
};

struct FileRecord {
    uint64_t tick;
    uint16_t action;
    uint8_t pressed;
    uint8_t source;
    uint32_t pad;
};

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 96);
static_assert(sizeof(FileRecord) == 16);

} // namespace

ReplayWriter::ReplayWriter(uint64_t seed, const char* table_slug, uint8_t nudge_level)
    : seed_(seed), nudge_level_(nudge_level) {
    std::snprintf(table_, sizeof(table_), "%s", table_slug);
}

ReplayWriter::~ReplayWriter() = default;

void ReplayWriter::add_edge(const ReplayEdge& edge) {
    edges_.push_back(edge);
}

bool ReplayWriter::finish(const char* out_path, uint64_t total_ticks) {
    FileHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(h.magic));
    h.version = 1;
    h.pad = 0;
    h.seed = seed_;
    std::memcpy(h.table, table_, sizeof(h.table));
    h.tick_count = total_ticks;
    h.nudge_level = nudge_level_;

    const bool ok = [&] {
        std::FILE* f = std::fopen(out_path, "wb");
        if (!f) {
            return false;
        }
        bool good = std::fwrite(&h, sizeof(h), 1, f) == 1;
        for (const ReplayEdge& e : edges_) {
            FileRecord r{e.tick, e.action, e.pressed, e.source, 0};
            good = good && std::fwrite(&r, sizeof(r), 1, f) == 1;
        }
        return std::fclose(f) == 0 && good;
    }();
    return ok;
}

bool ReplayReader::open(const char* path, ReplayReader& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    FileHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1 || std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) {
        std::fclose(f);
        return false;
    }
    out.header_.version = h.version;
    out.header_.seed = h.seed;
    std::memcpy(out.header_.table, h.table, sizeof(out.header_.table));
    out.header_.tick_count = h.tick_count;
    out.header_.nudge_level = h.nudge_level;

    FileRecord r;
    while (std::fread(&r, sizeof(r), 1, f) == 1) {
        out.edges_.push_back({r.tick, r.action, r.pressed, r.source});
    }
    std::fclose(f);
    return true;
}

} // namespace tb::sim
