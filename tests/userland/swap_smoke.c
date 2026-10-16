#include "../../kernel/include/api.h"
#include "../../sdk/include/bleskernos_vm.h"
#ifndef SWAP_EXPECT_ENABLED
#define SWAP_EXPECT_ENABLED 1
#endif
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(6000);
    bk_vm_statistics_t before,after;
    bool ok=bk_vm_statistics(0,&before);
    kprintf("[SWAPTEST] enabled=%u slots=%u\n",before.swap_enabled,before.swap_slots);
    int result=bk_app_run_sync("/SYSTEM/PROGRAMS/SWAPTEST.ELF",NULL);
    bk_sys_sleep_ms(1000);
    ok=ok && bk_vm_statistics(0,&after);
    kprintf("[SWAPTEST] child=%d resident=%u swapped=%u slots_used=%u out=%u in=%u faults=%u errors=%u\n",
        result,after.resident_pages,after.swapped_pages,after.swap_used,
        after.swap_writes,after.swap_reads,after.faults,after.swap_io_errors);
    if(SWAP_EXPECT_ENABLED) {
        ok=ok && result==0 && after.swap_writes>0 && after.swap_reads>0 &&
           after.swap_used==before.swap_used && after.virtual_pages==before.virtual_pages &&
           after.swap_io_errors==0;
        ok=ok && bk_vm_swap_enable(false) && bk_vm_statistics(0,&after) && !after.swap_enabled;
        ok=ok && bk_vm_swap_enable(true);
    } else ok=ok && !before.swap_enabled && result!=0 && !after.swap_used;
    kprintf("[SWAPTEST] DONE result=%s\n",ok?"PASS":"FAIL");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
