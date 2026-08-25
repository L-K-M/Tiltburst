#pragma once

namespace tb::detail {
[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg);
} // namespace tb::detail

#if defined(TB_DEBUG)
// Logs expression, file:line, and optional message via tb::log, then aborts.
#define TB_ASSERT(expr, ...)                                                                       \
    ((expr) ? (void)0 : tb::detail::assert_fail(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
#else
// Compiled out of release hot paths entirely; expr is NOT evaluated.
#define TB_ASSERT(expr, ...) ((void)0)
#endif

// Always-on check for startup / load / tool code (never in the sim hot path
// or audio callback). Active in all build types.
#define TB_CHECK(expr, ...)                                                                        \
    ((expr) ? (void)0 : tb::detail::assert_fail(#expr, __FILE__, __LINE__, "" __VA_ARGS__))
