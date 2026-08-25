#include <cstdio>

// M0 stub: the real CLI contract lands at M15 (09-table-format.md §10).
int main(int argc, char** argv) {
    (void)argc;
    const char* prog = argc > 0 ? argv[0] : "tb_validate";
    std::fprintf(stderr,
                 "usage: %s <table-dir> [--strict] [--json] [--migrate]\n"
                 "       not implemented yet; real CLI arrives at milestone M15\n",
                 prog);
    return 2;
}
