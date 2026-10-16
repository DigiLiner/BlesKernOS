#include "../../kernel/include/api.h"
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(6000);bk_perf_set_enabled(true);
    bk_proc_info_t a[32],b[32];bk_perf_snapshot_t snapshot;
    uint32_t n=bk_proc_snapshot(a,32);bk_perf_snapshot(&snapshot,true);
    uint32_t start=bk_sys_ticks();bk_sys_sleep_ms(10000);
    uint32_t elapsed=bk_sys_ticks()-start,m=bk_proc_snapshot(b,32);
    bk_perf_snapshot(&snapshot,false);
    bool vision_seen=false,ok=elapsed!=0;
    if(!elapsed)elapsed=1;
    for(uint32_t i=0;i<m;i++)for(uint32_t j=0;j<n;j++)if(a[j].pid==b[i].pid) {
        uint32_t cpu=(b[i].cpu_ticks-a[j].cpu_ticks)*100U/elapsed;
        if(b[i].name[0]=='V' && b[i].name[1]=='I' && b[i].name[2]=='S') {
            vision_seen=true;if(cpu>20U)ok=false;
        }
        kprintf("[IDLETEST] name=%s cpu=%u ticks=%u elapsed=%u\n",b[i].name,
            (b[i].cpu_ticks-a[j].cpu_ticks)*100U/elapsed,b[i].cpu_ticks-a[j].cpu_ticks,elapsed);
    }
    kprintf("[IDLETEST] frames=%u full=%u loops=%u compose_us=%u present_us=%u\n",
        snapshot.gui_frames,snapshot.gui_full_frames,snapshot.gui_loops,
        snapshot.compose_average_us,snapshot.present_average_us);
    kprintf("[IDLETEST] DONE result=%s\n",ok&&vision_seen?"PASS":"FAIL");
}
