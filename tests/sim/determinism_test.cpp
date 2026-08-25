#include "core/hash.h"
#include "sim/replay.h"
#include "sim/solver.h"
#include "support/data_path.h"
#include "support/tape.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// Procedural tape (16-testing-ci.md §2.4.3): PCG32(0x54425F44, fnv(slug)),
// gaps uniform in [50, 400], toggling bits {0,1,4,6,7,8}.
class ProceduralTape {
public:
    explicit ProceduralTape(const char* slug) : rng_() {
        rng_.seed(0x54425F44ull, tb::fnv1a64_str(slug));
    }

    uint32_t next_mask(uint32_t current) {
        tick_ += 50 + uint64_t(rng_.next_below(351));
        const uint32_t bit = 1u << bits_[rng_.next_below(6)];
        return current ^ bit;
    }

    uint64_t tick() const { return tick_; }

private:
    tb::Pcg32 rng_;
    uint64_t tick_ = 0;
    const uint32_t bits_[6] = {0, 1, 4, 6, 7, 8};
};

struct RunRecord {
    std::vector<uint64_t> hashes; // state_hash every 1,000 ticks
    uint64_t final_hash = 0;
};

// Drives `ticks` ticks, sampling the hash each 1,000. Buttons come either
// from a fixed procedural stream or from recorded edges.
RunRecord run_sim(tb::sim::SimState& s, int ticks, const std::vector<uint32_t>* buttons_by_tick) {
    tb::sim::Solver solver;
    RunRecord out;
    for (int i = 0; i < ticks; ++i) {
        tb::sim::TickInput in;
        if (buttons_by_tick != nullptr) {
            in.buttons = (*buttons_by_tick)[std::min(size_t(i), buttons_by_tick->size() - 1)];
        }
        solver.step(s, in);
        if ((i + 1) % 1000 == 0) {
            out.hashes.push_back(tb::sim::state_hash(s));
        }
    }
    out.final_hash = tb::sim::state_hash(s);
    return out;
}

// Expands button-word changes into replay edges (press/release per bit).
std::vector<tb::sim::ReplayEdge> words_to_edges(const std::vector<uint32_t>& words,
                                                uint8_t source) {
    std::vector<tb::sim::ReplayEdge> edges;
    uint32_t prev = 0;
    for (size_t t = 0; t < words.size(); ++t) {
        const uint32_t changed = prev ^ words[t];
        for (uint32_t bit = 0; bit < 16; ++bit) {
            if (changed & (1u << bit)) {
                edges.push_back(
                    {uint64_t(t), uint16_t(bit), uint8_t((words[t] >> bit) & 1u), source});
            }
        }
        prev = words[t];
    }
    return edges;
}

} // namespace

// Determinism.SameSeedSameHash: two identical runs of 10,000 ticks — same
// seed, same procedural input — produce identical hash-per-1000 sequences
// (04-milestones.md M2; canon §5.3).
TEST(det_determinism, same_seed_same_hash) {
    // Build the shared procedural input once.
    std::vector<uint32_t> words;
    {
        ProceduralTape tape("m2");
        uint32_t mask = 0;
        uint64_t last_tick = 0;
        while (last_tick < 10000) {
            mask = tape.next_mask(mask);
            words.resize(size_t(tape.tick()) + 1, mask);
            last_tick = tape.tick();
            // Fill any gap with the previous mask implicitly via resize.
        }
        words.resize(size_t(10001), words.empty() ? 0 : words.back());
        // Re-walk to stamp masks across the gaps (resize filled with back).
    }
    // Simpler: rebuild with gap fill.
    words.assign(10001, 0);
    {
        ProceduralTape tape("m2");
        uint32_t mask = 0;
        uint64_t cursor = 0;
        while (cursor <= 10000) {
            mask = tape.next_mask(mask);
            const uint64_t t = tape.tick();
            for (; cursor < t && cursor <= 10000; ++cursor) {
                words[size_t(cursor)] = mask;
            }
        }
    }

    tb::sim::SimState a;
    tb::sim::make_synthetic_scene(a, 424242);
    tb::sim::SimState b;
    tb::sim::make_synthetic_scene(b, 424242);

    const RunRecord ra = run_sim(a, 10000, &words);
    const RunRecord rb = run_sim(b, 10000, &words);

    ASSERT_EQ(ra.hashes.size(), rb.hashes.size());
    for (size_t i = 0; i < ra.hashes.size(); ++i) {
        ASSERT_EQ(ra.hashes[i], rb.hashes[i]) << "sample " << i;
    }
}

// Determinism.ReplayFileRoundTrip: record 5 s of play to .tbreplay,
// replay from file, hashes must match (04-milestones.md M2 demo path).
TEST(det_determinism, replay_file_round_trip) {
    // Shared procedural input over 5,000 ticks.
    std::vector<uint32_t> words(5001, 0);
    {
        ProceduralTape tape("m2");
        uint32_t mask = 0;
        uint64_t cursor = 0;
        while (cursor <= 5000) {
            mask = tape.next_mask(mask);
            const uint64_t t = tape.tick();
            for (; cursor < t && cursor <= 5000; ++cursor) {
                words[size_t(cursor)] = mask;
            }
        }
    }

    // Run A: play + record.
    const auto edges = words_to_edges(words, /*source=*/3); // synthetic
    tb::sim::ReplayWriter writer(424242, "test-lab", 2);
    for (const auto& e : edges) {
        writer.add_edge(e);
    }
    const std::filesystem::path path =
        tb::test::data_path(std::filesystem::path("build-tmp") / "m2_roundtrip.tbreplay");
    std::filesystem::create_directories(path.parent_path());
    ASSERT_TRUE(writer.finish(path.string().c_str(), 5000));

    // Load it back through the real reader.
    tb::sim::ReplayReader reader;
    ASSERT_TRUE(tb::sim::ReplayReader::open(path.string().c_str(), reader));
    ASSERT_EQ(reader.header().seed, 424242ull);

    // Re-inject: expand edge records into a buttons word per tick.
    std::vector<uint32_t> replay_words(5001, 0);
    uint32_t cur = 0;
    size_t e = 0;
    for (int t = 0; t <= 5000; ++t) {
        while (e < reader.edges().size() && reader.edges()[e].tick == uint64_t(t)) {
            const auto& edge = reader.edges()[e];
            const uint32_t bit = 1u << edge.action;
            cur = edge.pressed ? (cur | bit) : (cur & ~bit);
            ++e;
        }
        replay_words[size_t(t)] = cur;
    }

    // Run A and B states must match.
    tb::sim::SimState sa;
    tb::sim::make_synthetic_scene(sa, 424242);
    tb::sim::SimState sb;
    tb::sim::make_synthetic_scene(sb, 424242);

    const RunRecord ra = run_sim(sa, 5000, &words);
    const RunRecord rb = run_sim(sb, 5000, &replay_words);

    EXPECT_EQ(ra.final_hash, rb.final_hash);
    ASSERT_EQ(ra.hashes.size(), rb.hashes.size());
    for (size_t i = 0; i < ra.hashes.size(); ++i) {
        EXPECT_EQ(ra.hashes[i], rb.hashes[i]) << "sample " << i;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
