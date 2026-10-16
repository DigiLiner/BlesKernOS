#include "../../kernel/include/api.h"

void bleskernos_program_main(gui_desktop_t *desktop) {
    bk_sys_sleep_ms(6000);
    gfx_info_t info;
    bool ok=bk_gfx_info(&info) &&
        bk_gui_change_resolution(desktop,info.width,info.height,8);
    kprintf("[QUAKETEST] indexed display=%s\n",ok?"PASS":"FAIL");
    ok=bk_app_launch("/QUAKE/QUAKE.O",NULL) && ok;
    bk_sys_sleep_ms(15000);
    bk_memory_detail_t memory;bk_vm_statistics_t vm;
    if(bk_memory_detail(&memory) && bk_vm_statistics(0,&vm))
        kprintf("[QUAKETEST:MEM] heap_used_kib=%u heap_free_kib=%u vm_free_kib=%u swap_enabled=%u swap_used=%u\n",
                memory.heap_used/1024U,memory.heap_free/1024U,memory.vm_free/1024U,vm.swap_enabled,vm.swap_used);
    bk_proc_info_t processes[32];
    uint32_t count=bk_proc_snapshot(processes,32);
    bool running=false;
    for(uint32_t i=0;i<count;i++) {
        if(!bk_runtime_strcmp(processes[i].name,"quake")) running=true;
        kprintf("[QUAKETEST:PROC] pid=%u name=%s bytes=%u\n",
                processes[i].process_id,processes[i].name,processes[i].memory_bytes);
    }
    kprintf("[QUAKETEST] DONE result=%s\n",ok && running?"PASS":"FAIL");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
