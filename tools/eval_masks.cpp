// Quality evaluation of the mask refinement stage. The command-line behaviour and the dataset
// format are documented in eval_cli.h.
#include <cstdio>
#include <string>
#include <vector>

#include "eval_cli.h"

int main(int argc, char **argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mp_eval::run_cli(args, stdout, stderr);
}
