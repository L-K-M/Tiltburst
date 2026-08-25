#include "core/assert.h"

#include "core/log.h"

#include <cstdlib>

namespace tb::detail {

void assert_fail(const char* expr, const char* file, int line, const char* msg) {
    TB_LOG_ERROR("assert", "assert failed: {} at {}:{} {}", expr, file, line, msg);
    tb::log::flush_now();
    std::abort();
}

} // namespace tb::detail
