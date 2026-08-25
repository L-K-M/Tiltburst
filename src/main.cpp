#include "core/version.h"
#include "platform/app.h"

#include <cstdio>

int main(int argc, char** argv) {
    // §1 step 1: pure CLI parsing before any SDL call. --version prints
    // one line to stdout and exits 0 whatever else was passed; any other
    // problem prints the reason plus usage to stderr and exits 2
    // (05-engine-core.md §2.1).
    tb::app::CliOptions cli = tb::app::parse_cli(argc, argv);

    if (cli.version) {
        std::printf("tiltburst %s\n", tb::version_string());
        std::fflush(stdout);
        return 0;
    }
    if (!cli.error.empty()) {
        std::fprintf(stderr, "%s\n\n%s", cli.error.c_str(), tb::app::kUsage);
        return 2;
    }

    try {
        return tb::app::run(cli);
    } catch (const std::exception& e) {
        // Startup/load phases may throw (03-process.md §1.6); convert to a
        // clean failure — no exception may propagate out of main.
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
