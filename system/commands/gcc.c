#include "common.h"

static int run(int argc, char **argv) {
    bool compile_only = false;
    if (argc < 2) {
        kprintf("BlesKernOS GCC-compatible C frontend (TinyCC 0.9.28rc)\n");
        kprintf("Uso: gcc [opciones] archivo.c -o salida.o|salida.BEX\n");
        return 0;
    }
    for (int i = 1; i < argc; i++)
        if (command_is(argv[i], "-c") || command_is(argv[i], "-E") ||
            command_is(argv[i], "-S")) compile_only = true;
    return command_toolchain_run(NULL, argc, argv,
                                 compile_only ? NULL : "-nostdlib -r");
}

BK_COMMAND_MAIN(run)
