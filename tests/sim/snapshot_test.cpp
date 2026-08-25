#include "sim/snapshot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

// SnapshotBuffer.LatestWins: the reader observes a tick that is monotonic
// non-decreasing and eventually reaches the writer's final tick (04 M1).
TEST(unit_snapshot, latest_wins) {
    tb::SnapshotBuffer buffer;

    std::atomic<uint64_t> published{0};
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (uint64_t i = 1; i <= 100000; ++i) {
            tb::SimSnapshot snap;
            snap.tick = i;
            buffer.publish(snap);
            published.store(i, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    uint64_t last = 0;
    while (!done.load(std::memory_order_acquire)) {
        const uint64_t t = buffer.acquire_latest().tick;
        ASSERT_GE(t, last);
        last = t;
    }
    writer.join();

    // After the final publish, the reader must reach it (possibly on the
    // next acquire).
    uint64_t t = buffer.acquire_latest().tick;
    while (t < 100000) {
        t = buffer.acquire_latest().tick;
    }
    EXPECT_EQ(t, 100000u);
}

// SnapshotBuffer.NoTornRead: two writer threads hammering distinct fields
// would need a bigger snapshot; at M1 the snapshot is one u64, so the torn
// property is exercised as "reader always sees a value the writer actually
// wrote" — never an interleaved bit pattern.
TEST(unit_snapshot, no_torn_read) {
    tb::SnapshotBuffer buffer;

    constexpr uint64_t kIterations = 200000;
    std::thread writer([&] {
        for (uint64_t i = 1; i <= kIterations; ++i) {
            tb::SimSnapshot snap;
            snap.tick = i * 2; // even values only
            buffer.publish(snap);
        }
    });

    bool observed_any = false;
    while (!observed_any || writer.joinable()) {
        const uint64_t t = buffer.acquire_latest().tick;
        if (t != 0) {
            EXPECT_EQ(t % 2, 0u) << "read a value the writer never wrote";
            observed_any = true;
        }
        if (t >= kIterations * 2 - 4) {
            break;
        }
    }
    writer.join();
    EXPECT_EQ(buffer.acquire_latest().tick, kIterations * 2);
}
