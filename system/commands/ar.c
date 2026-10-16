#include "common.h"

static int run(int argc, char **argv) {
    if (argc < 3 && !(argc==2 && (command_is(argv[1],"--help") || command_is(argv[1],"--version")))) return command_error("ar", "uso: ar rcs biblioteca.a objetos...");
    return command_toolchain_run("/SYSTEM/DEVEL/BINUTILS/AR.EXE",
                                 argc, argv, NULL);
}

BK_COMMAND_MAIN(run)
