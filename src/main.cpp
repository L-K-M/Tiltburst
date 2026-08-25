#include "core/version.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char kUsage[] = "tiltburst - digital pinball\n"
                          "\n"
                          "Usage:\n"
                          "  tiltburst --version   Print the version and exit.\n"
                          "\n"
                          "All other options are not available yet; they arrive with later "
                          "milestones (05-engine-core.md §2).\n";

} // namespace

int main(int argc, char** argv) {
    // M0 implements exactly one flag: --version. Anything else is the
    // exit-2 "not available / skipped" contract of 04-milestones.md M0 and
    // 16-testing-ci.md §3.2/§5.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::string out = "tiltburst ";
            out += tb::version_string();
            out += "\n";
            fputs(out.c_str(), stdout);
            return 0;
        }
        fputs(kUsage, stderr);
        return 2;
    }

    fputs(kUsage, stderr);
    return 2;
}
