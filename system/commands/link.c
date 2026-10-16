#include "common.h"

static int run(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Uso: link objeto.o [biblioteca.a ...] -o programa.BEX\n");
        return 2;
    }
    return command_toolchain_run(NULL, argc, argv, "-nostdlib -r");
}

BK_COMMAND_MAIN(run)
