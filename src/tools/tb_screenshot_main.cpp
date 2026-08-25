#include <cstdio>

// M0 stub: the real CLI contract lands at M15 (06-rendering.md §15.2–§15.3).
int main(int argc, char** argv) {
    (void)argc;
    const char* prog = argc > 0 ? argv[0] : "tb_screenshot";
    std::fprintf(stderr,
                 "usage: %s <table-dir> --out <png-or-dir> [--width 1080] "
                 "[--height 1920]\n"
                 "       not implemented yet; real CLI arrives at milestone M15\n",
                 prog);
    return 2;
}
