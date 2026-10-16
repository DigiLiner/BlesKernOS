#include "../../kernel/include/api.h"
#ifndef RUNTIME_CASE
#define RUNTIME_CASE 0
#endif
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(6000);bool ok=true;
    int status=bk_app_run_sync("/WINEGATE/bin/ABI.ELF",NULL);
    kprintf("[RUNTIMETEST] ABI status=%d\n",status);if(status)ok=false;
    for(uint32_t i=RUNTIME_CASE?RUNTIME_CASE:1;i<=(RUNTIME_CASE?RUNTIME_CASE:33);i++) {
        char path[80];snprintf(path,sizeof(path),"/WINEGATE/bin/CASE%02u.ELF",i);
        kprintf("[RUNTIMETEST] starting case=%u\n",i);
        status=bk_app_run_sync(path,NULL);
        kprintf("[RUNTIMETEST] case=%u status=%d\n",i,status);if(status)ok=false;
    }
    status=bk_app_run_sync("/WINEGATE/bin/wine-tool.ELF","--version");
    kprintf("[RUNTIMETEST] upstream Wine version status=%d\n",status);if(status)ok=false;
    kprintf("[RUNTIMETEST] DONE result=%s\n",ok?"PASS":"FAIL");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
