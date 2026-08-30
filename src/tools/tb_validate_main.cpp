#include "table/validator.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// tb_validate (09-table-format.md §10): prints one diagnostic per line
// to stdout, or a JSON array with --json. Exit 0 = no errors (warnings
// allowed); 2 = errors present (--strict promotes warnings); 3 = IO.

namespace {

const char* kUsage = "usage: tb_validate <table-dir> [--strict] [--json] [--migrate]\n";

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path dir;
    bool strict = false;
    bool json = false;
    bool migrate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--strict") {
            strict = true;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--migrate") {
            migrate = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown flag: %s\n%s", arg.c_str(), kUsage);
            return 2;
        } else if (dir.empty()) {
            dir = arg;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }
    if (!std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "not a directory: %s\n", dir.string().c_str());
        return 3;
    }
    if (!std::filesystem::exists(dir / "table.json")) {
        std::fprintf(stderr, "no table.json in %s\n", dir.string().c_str());
        return 3;
    }
    if (migrate) {
        // 09 §9: no migrations exist at format_version 1.
        std::fprintf(stderr, "--migrate: no migrations exist at format_version 1\n");
    }

    const std::vector<tb::table::ValidationDiag> diags = tb::table::validate_pack(dir);

    if (json) {
        std::printf("[");
        for (size_t i = 0; i < diags.size(); ++i) {
            std::printf("%s%s", i > 0 ? ",\n" : "", diags[i].json().c_str());
        }
        std::printf("]\n");
    } else {
        for (const auto& dg : diags) {
            std::printf("%s\n", dg.line().c_str());
        }
    }

    bool failed = false;
    for (const auto& dg : diags) {
        if (dg.severity == "error" || strict) {
            failed = true;
        }
    }
    return failed ? 2 : 0;
}
