#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
#ifndef BLES_GL_TEST_ARGS
#define BLES_GL_TEST_ARGS "--size 640x480 -b build:duration=10 -b shading:shading=phong:duration=10 -b jellyfish:duration=15 -b refract:duration=30 -b bump:bump-render=normals:duration=10"
#endif
static bool same(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static bool live(const char *name) {
    /* Do not collect heap accounting at 5Hz while timing GPU presentation. */
    uint32_t count=bk_proc_count();
    for(uint32_t i=0;i<count;i++) {
        const task_t *process=bk_proc_get(i);
        if(process && same(process->name,name) &&
           process->state!=TASK_UNUSED && process->state!=TASK_ZOMBIE)return true;
    }
    return false;
}
static bool wait_exit(const char *name,unsigned polls) {
    bool seen=false;
    while(polls--) {
        if(live(name))seen=true;
        else if(seen)return true;
        bk_sys_sleep_ms(200);
    }
    return false;
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(2500);
    kprintf("[GALLIUM:BOOT] launch native ELF32, no Wine/X11\n");
    bool ok=bk_app_launch("/SYSTEM/PROGRAMS/GALLCHECK.ELF",NULL);
    kprintf("[GALLIUM:BOOT] shader/FBO check launch=%u\n",ok);
    bk_sys_sleep_ms(7000);
    ok=bk_app_launch("/SYSTEM/PROGRAMS/GALLTEST.ELF",NULL);
    kprintf("[GALLIUM:BOOT] probe launch=%u\n",ok);
    bk_sys_sleep_ms(6500);
    ok=bk_app_launch("/SYSTEM/PROGRAMS/GLMARK2.ELF",
        BLES_GL_TEST_ARGS);
    kprintf("[GALLIUM:BOOT] glmark2 launch=%u\n",ok);
    if(!ok || !wait_exit("GLMARK2.ELF",800)) {kprintf("[GALLIUM:BOOT] FAIL benchmark exit\n");return;}
    kprintf("[GALLIUM:BOOT] reopening native probe\n");
    if(!bk_app_launch("/SYSTEM/PROGRAMS/GALLTEST.ELF",NULL) || !wait_exit("GALLTEST.ELF",100)) {
        kprintf("[GALLIUM:BOOT] FAIL reopened probe exit\n");return;
    }
    if(live("GALLCHECK.ELF") || live("GALLTEST.ELF") || live("GLMARK2.ELF")) {
        kprintf("[GALLIUM:BOOT] FAIL applications still running\n");return;
    }
    kprintf("[GALLIUM:BOOT] all native applications exited\n");
    bk_sys_sleep_ms(2000);
    kprintf("[GALLIUM:BOOT] desktop alive after cleanup\n");
}
