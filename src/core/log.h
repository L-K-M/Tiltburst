#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

// Tiny thread-safe logger over fmt (05-engine-core.md §12). No third-party
// logging library. The hot path formats into a stack buffer and copies into
// a global ring under a spinlock held only for the memcpy: no heap
// allocation, no file I/O, no syscalls on the calling thread.
namespace tb {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

namespace log {

// Ring-only init (§1 step 2); the file sink attaches later on main.
void init(LogLevel runtime_threshold);
void set_file(const std::filesystem::path& path);
void shutdown();

bool enabled(LogLevel level);

// `thread` is a fixed 5-char tag: "main ", "sim  ", "audio", "rawin",
// "other".
void message(const char* thread, LogLevel level, fmt::string_view fmt, fmt::format_args args);

template <typename... Args>
void emit(const char* thread,
          LogLevel level,
          fmt::format_string<Args...> fmt,
          const Args&... args) {
    if (!enabled(level)) {
        return;
    }
    message(thread, level, fmt.get(), fmt::make_format_args(args...));
}

// Main thread drains new ring entries into the file once per frame.
void drain_to_file();
void flush_now();

// At most one emission per second per call site (05 §12).
bool warn_ratelimited(uint64_t& last_emission_ns);

} // namespace log

#define TB_LOG_TRACE(thread_tag, ...)                                                              \
    ::tb::log::emit(thread_tag, ::tb::LogLevel::Trace, __VA_ARGS__)
#define TB_LOG_DEBUG(thread_tag, ...)                                                              \
    ::tb::log::emit(thread_tag, ::tb::LogLevel::Debug, __VA_ARGS__)
#define TB_LOG_INFO(thread_tag, ...) ::tb::log::emit(thread_tag, ::tb::LogLevel::Info, __VA_ARGS__)
#define TB_LOG_WARN(thread_tag, ...) ::tb::log::emit(thread_tag, ::tb::LogLevel::Warn, __VA_ARGS__)
#define TB_LOG_ERROR(thread_tag, ...)                                                              \
    ::tb::log::emit(thread_tag, ::tb::LogLevel::Error, __VA_ARGS__)

// Ratelimited warning keyed on the call site's static counter.
#define TB_LOG_WARN_RATELIMITED(tag, ...)                                                          \
    do {                                                                                           \
        static uint64_t tb_last_emit_ns = 0;                                                       \
        if (::tb::log::warn_ratelimited(tb_last_emit_ns)) {                                        \
            TB_LOG_WARN(tag, __VA_ARGS__);                                                         \
        }                                                                                          \
    } while (0)

} // namespace tb
