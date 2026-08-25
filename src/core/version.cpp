#include "core/version.h"

// Release builds override the version from the git tag via
// -DTB_RELEASE_TAG (16-testing-ci.md §3.3); dev builds fall back to the
// scaffold default.
#ifndef TB_RELEASE_TAG
#define TB_RELEASE_TAG "0.1.0"
#endif

namespace tb {

const char* version_string() {
    return TB_RELEASE_TAG;
}

} // namespace tb
