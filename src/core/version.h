#pragma once

namespace tb {

// Single source of product version. Overridden at release time via
// -DTB_RELEASE_TAG (16-testing-ci.md §3.3).
const char* version_string();

} // namespace tb
