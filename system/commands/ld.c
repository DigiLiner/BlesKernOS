#include "common.h"

static int run(int argc, char **argv) {
    if (argc < 3 && !(argc==2 && (command_is(argv[1],"--help") || command_is(argv[1],"--version")))) return command_error("ld", "uso: ld objetos... -o salida.BEX");
    return command_toolchain_run("/SYSTEM/DEVEL/BINUTILS/LD.EXE",
                                 argc, argv, NULL);
}

BK_COMMAND_MAIN(run)
