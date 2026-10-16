#include "../kernel/include/api.h"
#define WINE_LAUNCHER "/WINEGATE/bin/wine-launch.ELF"

void bleskernos_program_main(gui_desktop_t *desktop) {
    const char *path = bk_proc_launch_arg();
    (void)desktop;
    if (!path || !path[0]) path = "winecfg";
    if (!bk_app_launch(WINE_LAUNCHER, path))
        bk_sys_log("[WINE] no se pudo iniciar Wine 11 mediante wine-launch.ELF");
}
