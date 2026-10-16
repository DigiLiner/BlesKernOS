#include "common.h"

static int run(int argc UNUSED, char **argv UNUSED) {
#if defined(BLES_ARCH_ARM32_RPI3)
    kprintf("BlesKernOS 0.9 armv7-a API-%u\n", bk_sys_api_version());
#else
    kprintf("BlesKernOS 0.9 i386 API-%u\n", bk_sys_api_version());
#endif
    return 0;
}

BK_COMMAND_MAIN(run)
