#include <cstdio>

// M0 stub: the real CLI contract lands at M15 (14-authoring-guide.md §8.2).
int main(int argc, char** argv) {
    (void)argc;
    const char* prog = argc > 0 ? argv[0] : "tb_autoplay";
    std::fprintf(stderr,
                 "usage: %s <table-dir> --runs N --skill {0|1|2} --seed S "
                 "[--balls 3 | --seconds 300]\n"
                 "       not implemented yet; real CLI arrives at milestone M15\n",
                 prog);
    return 2;
}
