#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    bk_sys_sleep_ms(3000);
    int low=bk_app_run_sync("/WINEGATE/bin/LOWDOS.ELF",NULL);
    kprintf("[WINE-SYNC] low-dos status=%d\n",low);
    int stress=0;
    for(unsigned i=0;i<3;i++) {
        int result=bk_app_run_sync("/WINEGATE/bin/FUTEXSTRESS.ELF",NULL);
        kprintf("[WINE-SYNC] stress run=%u status=%d\n",i+1,result);
        if(result)stress=result;
    }
    kprintf("[WINE-SYNC] DONE result=%s\n",!low && !stress?"PASS":"FAIL");
    bk_proc_exit_status(low || stress);
}
