#pragma once

#include <cstdint>
#include <vector>

// .tbreplay record/replay files (05-engine-core.md §13): binary,
// little-endian, 96-byte header + 16-byte edge records.
namespace tb::sim {

struct ReplayEdge {
    uint64_t tick = 0;
    uint16_t action = 0;
    uint8_t pressed = 0;
    uint8_t source = 0; // as produced; replay injects with source = 4
};

struct ReplayHeader {
    uint16_t version = 1;
    uint64_t seed = 0;
    char table[64]{};
    uint64_t tick_count = 0;
    uint8_t nudge_level = 2;
};

class ReplayWriter {
public:
    explicit ReplayWriter(uint64_t seed, const char* table_slug, uint8_t nudge_level);
    ~ReplayWriter();
    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    void add_edge(const ReplayEdge& edge);
    bool finish(const char* out_path, uint64_t total_ticks);

private:
    std::vector<ReplayEdge> edges_;
    uint64_t seed_ = 0;
    uint8_t nudge_level_ = 2;
    char table_[64]{};
};

class ReplayReader {
public:
    static bool open(const char* path, ReplayReader& out);

    const ReplayHeader& header() const { return header_; }

    // Edge for exact-tick injection: edges are sorted by tick.
    const std::vector<ReplayEdge>& edges() const { return edges_; }

private:
    ReplayHeader header_{};
    std::vector<ReplayEdge> edges_;
};

} // namespace tb::sim
