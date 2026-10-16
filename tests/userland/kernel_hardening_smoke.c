#include "../../kernel/include/api.h"
#include "../../kernel/libc/include/string.h"
#include "../../kernel/libc/include/stdlib.h"

static bool failed;
static void check(bool ok,const char *name) {
    kprintf("[HARDENINGTEST] %s: %s\n",name,ok?"PASS":"FAIL");
    if(!ok)failed=true;
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0":"=r"(flags));
    check(!(flags&0x3000U),"Ring 3 IOPL zero");
    char *buffer=bk_sys_alloc(8);
    check(buffer!=NULL,"user allocation");
    if(buffer) {
        kmemset(buffer,'Q',8);
        check(kmemcpy((void *)0x00100000U,buffer,4)==NULL,"kernel text write refused");
        check(kmemcpy(buffer,(void *)0x00100000U,4)==NULL,"kernel text read refused");
        check(memset((void *)0x00300000U,0,4)==NULL,"kernel BSS write refused");
        check(kmemcpy(buffer-1,buffer,1)==NULL,"heap header write refused");
        check(memcpy(buffer,"hello",6)==buffer && !strcmp(buffer,"hello"),"valid copy and compare");
        check(strcpy((char *)0x00100000U,"test")==NULL,"string kernel write refused");
        check(strncpy(buffer,"hello",8192)==NULL && !strcmp(buffer,"hello"),"string output extent refused");
        check(!mm_set_allocation_owner(buffer,0),"ownership change refused");
        kfree((void *)0x00100000U);
        check(buffer[0]=='h',"invalid free contained");
        char *grown=bk_sys_realloc(buffer,128);
        check(grown!=NULL && !strcmp(grown,"hello"),"owned realloc preserves data");
        if(grown)buffer=grown;
        bk_sys_free(buffer);
    }
    system_memory_info_t memory;
    check(bk_sys_memory_info(&memory) && memory.used_bytes+memory.free_bytes+memory.reserved_bytes==memory.total_bytes,"memory accounting");
    kprintf("[HARDENINGTEST] DONE result=%s\n",failed?"FAIL":"PASS");
    while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);
}
