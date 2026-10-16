#include "../../kernel/include/api.h"
#include "../../sdk/include/bleskernos_vm.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(6000);
    bk_memory_detail_t before={0},after={0};
    bk_vm_statistics_t vm_before={0},vm_after={0};
    bool ok=bk_memory_detail(&before) && bk_vm_statistics(0,&vm_before);
    int status=bk_app_run_sync("/SYSTEM/PROGRAMS/VMMETA.ELF",NULL);
    bk_sys_sleep_ms(1000);
    ok=ok && bk_memory_detail(&after) && bk_vm_statistics(0,&vm_after);
    kprintf("[VMMETATEST] child=%d vm_free_before=%u vm_free_after=%u virtual_before=%u virtual_after=%u\n",
            status,before.vm_free,after.vm_free,vm_before.virtual_pages,vm_after.virtual_pages);
    ok=ok && !status && after.vm_free>=before.vm_free &&
        vm_before.virtual_pages==vm_after.virtual_pages;
    kprintf("[VMMETATEST] DONE result=%s\n",ok?"PASS":"FAIL");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
