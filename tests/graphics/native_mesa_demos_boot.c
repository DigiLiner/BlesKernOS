#define BK_API_IMPLEMENTATION
#ifndef BLES_MESA_TRACE
#define BLES_MESA_TRACE ""
#endif
#include "../../kernel/include/api.h"
static bool same(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static bool live(const char *name) {
    /* Only names/states are needed. A process-memory census walks the global
     * heap under its IRQ lock and perturbs the renderer being measured. */
    uint32_t count=bk_proc_count();
    for(uint32_t i=0;i<count;i++) {
        const task_t *process=bk_proc_get(i);
        if(process && same(process->name,name) &&
           process->state!=TASK_UNUSED && process->state!=TASK_ZOMBIE)return true;
    }
    return false;
}
static bool run(const char *name,const char *label,const char *backend,const char *args) {
    kprintf("[MESADEMOS:BOOT] RUN %s %s\n",label,backend);
    if(!bk_app_launch(name,args))return false;
    bool seen=false;
    for(unsigned i=0;i<200;i++) {
        if(live(label))seen=true;
        else if(seen){bk_sys_sleep_ms(1000);return true;}
        bk_sys_sleep_ms(200);
    }
    return false;
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    (void)desktop;bk_sys_sleep_ms(2500);
#define DEMO(scene,label) \
    if(!run("/SYSTEM/PROGRAMS/MD" scene ".ELF","MD" scene ".ELF","mesa35","--backend mesa35 --size 640x480 --seconds 20" BLES_MESA_TRACE))goto fail; \
    if(!run("/SYSTEM/PROGRAMS/MD" scene ".ELF","MD" scene ".ELF","gallium","--backend gallium --size 640x480 --seconds 20"))goto fail;
#ifndef BLES_MESA_PACING_ONLY
#define REFERENCES(scene,label) \
    if(!run("/SYSTEM/PROGRAMS/MD" scene ".ELF","MD" scene ".ELF","software","--backend software --size 640x480 --seconds 5"))goto fail; \
    if(!run("/SYSTEM/PROGRAMS/MD" scene ".ELF","MD" scene ".ELF","fixedgpu","--backend mesa35 --size 640x480 --fixed-time --snapshot /SYSTEM/USER/" label "-gpu.ppm"))goto fail; \
    if(!run("/SYSTEM/PROGRAMS/MD" scene ".ELF","MD" scene ".ELF","fixedcpu","--backend software --size 640x480 --close-check --fixed-time --snapshot /SYSTEM/USER/" label "-cpu.ppm"))goto fail;
#else
#define REFERENCES(scene,label)
#endif
#if !BLES_MESA_SCENE || BLES_MESA_SCENE==1
    DEMO("GEARS","gears") REFERENCES("GEARS","gears")
#endif
#if !BLES_MESA_SCENE || BLES_MESA_SCENE==2
    DEMO("GLOSS","gloss") REFERENCES("GLOSS","gloss")
#endif
#if !BLES_MESA_SCENE || BLES_MESA_SCENE==3
    DEMO("MORPH3D","morph3d") REFERENCES("MORPH3D","morph3d")
#endif
    if(!run("/SYSTEM/PROGRAMS/LBSTRI.BEX","LBSTRI.BEX","lbs",NULL))goto fail;
    bk_sys_sleep_ms(2000);
    kprintf("[MESADEMOS:BOOT] COMPLETE desktop alive after cleanup\n");return;
fail:kprintf("[MESADEMOS:BOOT] FAIL lifecycle\n");
}
