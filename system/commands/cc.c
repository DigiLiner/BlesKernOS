#include "common.h"

static int run(int argc, char **argv) {
    bool compile_only = false;
    if (argc < 2) return command_error("cc", "uso: cc [opciones] archivo.c");
    for (int i = 1; i < argc; i++)
        if (command_is(argv[i], "-c") || command_is(argv[i], "-E") ||
            command_is(argv[i], "-S")) compile_only = true;
    return command_toolchain_run(NULL, argc, argv,
                                 compile_only ? NULL : "-nostdlib -r");
}

BK_COMMAND_MAIN(run)
