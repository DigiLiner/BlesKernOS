#include "../../kernel/include/api.h"

static bool failed;
static void check(bool ok, const char *name) {
    kprintf("[RAMTEST] %s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failed = true;
}
static void report(const char *stage) {
    bk_memory_detail_t m;
    if (bk_memory_detail(&m))
        kprintf("[RAMTEST:MEM] stage=%s used_kib=%u free_kib=%u heap_free_kib=%u vm_free_kib=%u\n",
                stage,m.used/1024U,m.free/1024U,m.heap_free/1024U,m.vm_free/1024U);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    bk_sys_sleep_ms(6000);
    report("idle");
    bk_memory_subsystem_t categories[BK_MEMORY_SUBSYSTEMS];
    uint32_t categories_count=bk_memory_subsystems(categories,BK_MEMORY_SUBSYSTEMS);
    for(uint32_t i=0;i<categories_count;i++)
        kprintf("[RAMTEST:SUBSYSTEM] id=%u bytes=%u blocks=%u\n",i,categories[i].bytes,categories[i].blocks);
    bk_memory_detail_t before,after;
    check(bk_memory_detail(&before),"initial memory snapshot");
    uint8_t *large = bk_sys_alloc(6U * 1024U * 1024U);
    check(large != NULL,"6 MiB native allocation");
    if (large) {
        for (uint32_t i=0;i<6U*1024U*1024U;i+=4096U) large[i]=(uint8_t)(i/4096U);
        uint8_t *smaller=bk_sys_realloc(large,1024U*1024U);
        check(smaller==large,"shrink preserves address");
        if (smaller) large=smaller;
        bool intact=true;
        for (uint32_t i=0;i<1024U*1024U;i+=4096U)
            if (large[i]!=(uint8_t)(i/4096U)) intact=false;
        check(intact,"large allocation data intact");
        bk_sys_free(large);
    }
    check(bk_memory_detail(&after) && after.heap_used<=before.heap_used+65536U,
          "large allocation reclaimed");
    check(bk_app_run_sync("/SYSTEM/PROGRAMS/GFXALLOC.ELF",NULL)==0,"256 Linux graphics open/close cycles");
    check(bk_memory_detail(&after) && after.heap_used<=before.heap_used+65536U,"graphics contexts released");
    bk_proc_info_t processes[32];
    uint32_t count=bk_proc_snapshot(processes,32);
    bool wine=false;
    for(uint32_t i=0;i<count;i++) {
        kprintf("[RAMTEST:PROC] pid=%u name=%s bytes=%u\n",processes[i].process_id,
                processes[i].name,processes[i].memory_bytes);
        if(!bk_runtime_strcmp(processes[i].name,"wineserver") ||
           !bk_runtime_strcmp(processes[i].name,"wine-launch") ||
           !bk_runtime_strcmp(processes[i].name,"XFBDEV"))wine=true;
    }
    check(!wine,"no Wine background processes");
    gui_window_t *window=bk_gui_create_window(desktop,220,100,320,180,"Prueba RAM y colores");
    check(window!=NULL,"native window creation");
    gfx_info_t original;
    check(bk_gfx_info(&original),"graphics info");
    gfx_display_mode_t modes[128];uint32_t mode_count=0;
    bool listed=gfx_list_all_display_modes(modes,128,&mode_count);
    check(listed && mode_count>0,"display mode catalogue");
    uint8_t depths[]={8,16,24,32};
    for(uint32_t d=0;d<4;d++) {
        bool available=false;
        for(uint32_t i=0;i<mode_count;i++)
            if(modes[i].width==original.width && modes[i].height==original.height &&
               modes[i].bpp==depths[d])available=true;
        if(!available)continue;
        bool ok=bk_gui_change_resolution(desktop,original.width,original.height,depths[d]);
        gfx_info_t info;
        ok=ok && bk_gfx_info(&info) && info.bpp==depths[d];
        kprintf("[RAMTEST:DEPTH] bpp=%u result=%s\n",depths[d],ok?"PASS":"FAIL");
        check(ok,"change supported color depth");
        bk_sys_sleep_ms(800);
    }
    check(bk_gui_change_resolution(desktop,original.width,original.height,original.bpp),"restore display mode");
    if(window)bk_gui_close_window(window);
    check(bk_app_launch("/SYSTEM/PROGRAMS/FILE.BEX",NULL),"open file manager");
    check(bk_app_launch("/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",NULL),"open text editor");
    check(bk_app_launch("/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX",NULL),"open process manager");
    bk_sys_sleep_ms(4000);
    report("three-apps");
    count=bk_proc_snapshot(processes,32);
    for(uint32_t i=0;i<count;i++)
        kprintf("[RAMTEST:APPS] pid=%u name=%s bytes=%u\n",processes[i].process_id,
                processes[i].name,processes[i].memory_bytes);
    kprintf("[RAMTEST] DONE result=%s\n",failed?"FAIL":"PASS");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
