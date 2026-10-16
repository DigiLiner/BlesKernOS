#include "common.h"

static int run(int argc, char **argv) {
    /* La terminal de BlesKernOS también usa convenciones DOS. Aceptar /f
     * evita que GNU Make lo interprete como el nombre de un objetivo. */
    for (int i = 1; i < argc; i++) {
        if (command_is(argv[i], "/f") || command_is(argv[i], "/F"))
            argv[i] = "-f";
    }
    return command_toolchain_run("/SYSTEM/DEVEL/MAKE/MAKE.EXE",
                                 argc, argv, NULL);
}

BK_COMMAND_MAIN(run)
