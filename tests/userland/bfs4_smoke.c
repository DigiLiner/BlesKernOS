#include "../../kernel/libc/include/stdio.h"
#include "../../sdk/include/bleskernos_api.h"
extern bool bk_file_space(uint64_t *,uint64_t *);
static bool failed;
static void check(bool ok,const char *text) {
    if(!ok){failed=true;kprintf("[FSTEST] FAIL %s\n",text);}
}
void bleskernos_program_main(bk_gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(6000);
    uint64_t total,before,after;uint8_t data[16384];char path[64],moved[64];
    check(bk_file_space(&total,&before),"space baseline");
    check(bk_file_mkdir("/PHASE3TEST"),"mkdir");
    for(uint32_t round=0;round<4;round++) {
        for(uint32_t i=0;i<12;i++) {
            snprintf(path,sizeof(path),"/PHASE3TEST/F%u.BIN",i);
            snprintf(moved,sizeof(moved),"/PHASE3TEST/M%u.BIN",i);
            uint32_t size=513U+(i+1U)*1024U;
            for(uint32_t j=0;j<size;j++)data[j]=(uint8_t)(round*19U+i*37U+j);
            check(bk_file_write_all(path,data,size),"create/write");
            check(bk_file_rename(path,moved),"rename");
            void *received=NULL;uint32_t got=0;
            check(bk_file_read_all(moved,&received,&got) && got==size &&
                  !bk_runtime_memcmp(data,received,size),"read bytes");
            if(received)bk_sys_free(received);
            check(bk_file_write_all(moved,data,37),"truncate/overwrite");
            check(bk_file_remove(moved),"delete");
        }
        kprintf("[FSTEST] round=%u completed\n",round);
    }
    check(bk_file_remove("/PHASE3TEST"),"remove directory");
    check(bk_file_space(&total,&after),"space after test");
    kprintf("[FSTEST] global free blocks before=%u after=%u (background services may write)\n",
        (uint32_t)(before/4096U),(uint32_t)(after/4096U));
    bk_volume_check_report_t report;bk_file_entry_t removed;
    check(!bk_file_stat("/PHASE3TEST",&removed),"test tree fully removed");
    bool scanned=bk_device_check_volume(&report);
    kprintf("[FSTEST] scan=%u errors=%u lost=%u cross=%u invalid=%u size=%u dirs=%u free=%u\n",
        scanned,report.errors,report.lost_clusters,report.crosslinked_clusters,
        report.invalid_chains,report.size_mismatches,report.directory_errors,report.free_clusters);
    check(scanned && report.completed && !report.errors && !report.lost_clusters &&
          !report.crosslinked_clusters,"no leaked allocations or dangling references");
    kprintf("[FSTEST] DONE result=%s\n",failed?"FAIL":"PASS");
}
