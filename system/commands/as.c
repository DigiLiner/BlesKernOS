#include "common.h"

static int run(int argc, char **argv) {
    if (argc < 2) return command_error("as", "uso: as fuente.S -o objeto.o");
    return command_toolchain_run("/SYSTEM/DEVEL/BINUTILS/AS.EXE",
                                 argc, argv, NULL);
}

BK_COMMAND_MAIN(run)
