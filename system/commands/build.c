#include "common.h"

static int run(int argc, char **argv) {
    if (argc != 3) {
        kprintf("Uso: build fuente.c programa.BEX\n");
        kprintf("Incluye el SDK y enlaza LIBBLESK.A automaticamente.\n");
        return 2;
    }
    return command_build_native(argv[1], argv[2]);
}

BK_COMMAND_MAIN(run)
