#include "core/log.h"

#include "core/time.h"

#include <atomic>
#include <cstdio>

namespace tb::log {

namespace {

constexpr size_t kRingEntries = 4096;
constexpr size_t kMsgBytes = 246;

struct Entry {
    uint64_t ts_ns;
    uint8_t level;
    const char* thread; // static 5-char tag, never freed
    char msg[246];
};

Entry g_ring[kRingEntries]{};
std::atomic<uint64_t> g_write_pos{0};
std::atomic<uint64_t> g_drained{0};
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

std::atomic<LogLevel> g_threshold{LogLevel::Info};

std::FILE* g_file = nullptr;
std::filesystem::path g_file_path;

uint64_t g_start_ns = 0;

const char* level_tag(LogLevel l) {
    switch (l) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO  ";
    case LogLevel::Warn:
        return "WARN  ";
    case LogLevel::Error:
        return "ERROR ";
    }
    return "?";
}

void write_entry(const Entry& e) {
    if (!g_file) {
        return;
    }
    const double secs = double(e.ts_ns - g_start_ns) * 1e-9;
    std::fprintf(
        g_file, "[+%12.6f] [%s] [%s] %s\n", secs, level_tag(LogLevel(e.level)), e.thread, e.msg);
    // warn+ flushes so a crash leaves the evidence on disk.
    if (e.level >= uint8_t(LogLevel::Warn)) {
        std::fflush(g_file);
    }
}

} // namespace

void init(LogLevel runtime_threshold) {
    g_threshold.store(runtime_threshold, std::memory_order_relaxed);
    g_start_ns = tb_now_ns();
}

bool enabled(LogLevel level) {
    return uint8_t(level) >= uint8_t(g_threshold.load(std::memory_order_relaxed));
}

void message(const char* thread, LogLevel level, fmt::string_view fmt, fmt::format_args args) {
    Entry e{};
    e.ts_ns = tb_now_ns();
    e.level = uint8_t(level);
    e.thread = thread;
    auto out = fmt::vformat_to_n(e.msg, kMsgBytes - 1, fmt, args);
    *out.out = '\0';

    while (g_lock.test_and_set(std::memory_order_acquire)) {
        cpu_pause();
    }
    const uint64_t pos = g_write_pos.fetch_add(1, std::memory_order_relaxed);
    g_ring[pos % kRingEntries] = e;
    g_lock.clear(std::memory_order_release);

    // Warn+ goes straight to stderr so CI sees it even without the file sink.
    if (level >= LogLevel::Warn) {
        std::fprintf(stderr, "[%s] [%s] %s\n", level_tag(level), thread, e.msg);
    }
}

void set_file(const std::filesystem::path& path) {
    // Called from main during boot, before the sim/audio threads exist.
    if (g_file) {
        std::fclose(g_file);
    }
    g_file_path = path;
    g_file = std::fopen(path.string().c_str(), "wb");
}

void drain_to_file() {
    if (!g_file) {
        return;
    }
    const uint64_t w = g_write_pos.load(std::memory_order_acquire);
    while (g_drained.load(std::memory_order_relaxed) < w) {
        const uint64_t r = g_drained.fetch_add(1, std::memory_order_relaxed);
        // The producer may have lapped us only after 4096 entries in one
        // frame; accept the loss rather than block any thread.
        write_entry(g_ring[r % kRingEntries]);
    }
    std::fflush(g_file);
}

void flush_now() {
    drain_to_file();
    if (g_file) {
        std::fflush(g_file);
    }
    std::fflush(stderr);
}

void shutdown() {
    flush_now();
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

bool warn_ratelimited(uint64_t& last_emission_ns) {
    const uint64_t now = tb_now_ns();
    if (now >= last_emission_ns && now - last_emission_ns < 1'000'000'000ull) {
        return false;
    }
    last_emission_ns = now;
    return true;
}

} // namespace tb::log
