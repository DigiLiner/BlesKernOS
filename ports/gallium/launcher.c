/* SPDX-License-Identifier: MIT */
#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    const char *args=bk_proc_launch_arg();
    if(!args || !*args)args="--size 640x480 -b build -b shading:shading=phong -b jellyfish -b refract -b bump:bump-render=normals";
    if(!bk_app_launch("/SYSTEM/PROGRAMS/GLMARK2.ELF",args))
        kprintf("[GLMARK2] no se pudo iniciar el benchmark nativo\n");
}
