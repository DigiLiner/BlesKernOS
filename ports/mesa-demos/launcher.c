/* SPDX-License-Identifier: MIT */
#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
#ifndef MESA_DEMO
#error MESA_DEMO must name an installed scene
#endif
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    const char *args=bk_proc_launch_arg();
    if(!args || !*args)args="--backend gallium --size 640x480";
    if(!bk_app_launch("/SYSTEM/PROGRAMS/MD" MESA_DEMO ".ELF",args))
        kprintf("[MESADEMOS] no se pudo iniciar " MESA_DEMO "\n");
}
