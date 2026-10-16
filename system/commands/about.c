#include "common.h"

static int run(int argc UNUSED, char **argv UNUSED) {
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("BlesKernOS 0.9 ARM32 (C) Bles.INC 2026\n");
#else
    kprintf("BlesKernOS 0.9 (C) Bles.INC 2026\n");
#endif
    return 0;
}

BK_COMMAND_MAIN(run)
