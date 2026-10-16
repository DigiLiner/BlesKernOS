#include "common.h"

static int run(int argc, char **argv) {
    return command_toolchain_run("/SYSTEM/DEVEL/BINUTILS/ADDR2LINE.EXE",
                                 argc, argv, NULL);
}

BK_COMMAND_MAIN(run)
