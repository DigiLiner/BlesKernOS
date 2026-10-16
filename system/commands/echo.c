#include "common.h"

static int run(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) bk_console_putchar(' ');
        bk_console_write(argv[i]);
    }
    bk_console_putchar('\n');
    return 0;
}

BK_COMMAND_MAIN(run)
