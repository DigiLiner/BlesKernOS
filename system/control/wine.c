#include "kernel/include/api.h"

/* Control Panel opens Wine's own configuration dialog, including About. */
void bleskernos_program_main(gui_desktop_t *desktop) {
    if (!bk_app_execute_path(desktop, "/SYSTEM/PROGRAMS/WINE.BEX"))
        bk_sys_log("[WINE] no se pudo abrir winecfg desde el Panel de control");
    bk_proc_exit();
}
