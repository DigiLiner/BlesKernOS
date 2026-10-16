#include "../../kernel/include/api.h"
#ifndef USBPERF_SETTLE_MS
#define USBPERF_SETTLE_MS 20000U
#endif
static uint8_t data[124U*1024U];
static uint32_t hash(uint32_t h,uint32_t n) {
    for(uint32_t i=0;i<n;i++)h=(h^data[i])*16777619U;
    return h;
}
void bleskernos_program_main(gui_desktop_t *desktop UNUSED) {
    kprintf("[USBPERF] START\n");
    bk_sys_sleep_ms(USBPERF_SETTLE_MS);
    bool ok=true;
    for(uint32_t pass=0;pass<2;pass++) {
        uint32_t start=bk_sys_ticks(),total=0,h=2166136261U;
        kprintf("[USBPERF] OPEN pass=%u\n",pass);
        int fd=bk_file_open("/SYSTEM/PROGRAMS/USBPERF.DAT",0);
        if(fd<0){ok=false;break;}
        for(;;) {
            int got=bk_file_read(fd,data,sizeof(data));
            if(got<0){ok=false;break;}
            if(!got)break;
            total+=(uint32_t)got;h=hash(h,(uint32_t)got);
        }
        bk_file_close(fd);
        bool valid=total==1048576U && h==0x7f65ea70U;
        if(!valid)ok=false;
        kprintf("[USBPERF] pass=%u bytes=%u hash=%x ticks=%u hz=%u valid=%u\n",
            pass,total,h,bk_sys_ticks()-start,bk_sys_tick_frequency(),valid?1U:0U);
    }
    ok=bk_app_launch("/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX",NULL)&&ok;
    bk_sys_sleep_ms(2000);
    ok=bk_app_launch("/SYSTEM/PROGRAMS/FILE.BEX",NULL)&&ok;
    bk_sys_sleep_ms(10000);
    bk_proc_info_t tasks[32];uint32_t count=bk_proc_snapshot(tasks,32);
    bool monitor=false,browser=false;
    for(uint32_t i=0;i<count;i++) {
        kprintf("[USBPERF:PROC] %s\n",tasks[i].name);
        if(!bk_runtime_strcmp(tasks[i].name,"processmgr"))monitor=true;
        if(!bk_runtime_strcmp(tasks[i].name,"filebrowser") ||
           !bk_runtime_strcmp(tasks[i].name,"FILE.BEX"))browser=true;
    }
    ok=ok&&monitor&&browser;
    kprintf("[USBPERF] DONE result=%s tasks=%u\n",ok?"PASS":"FAIL",count);
    bk_proc_exit();
}
