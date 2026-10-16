#include "common.h"

static int run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Uso: compile fuente.c -o objeto.o [opciones]\n");
        return 2;
    }
    return command_toolchain_run(NULL, argc, argv, "-c");
}

BK_COMMAND_MAIN(run)
