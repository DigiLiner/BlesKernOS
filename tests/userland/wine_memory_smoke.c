#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
#include "../../kernel/include/keyboard.h"
static volatile unsigned cue;
static bool event(gui_window_t *w,const gui_event_t *e,void *ctx) {
    (void)w;(void)ctx;
    if(e->type==GUI_EVENT_KEY && e->key==KEY_F9){cue=1;return true;}
    if(e->type==GUI_EVENT_KEY && e->key==KEY_F8){cue=2;return true;}
    if(e->type==GUI_EVENT_KEY && e->key==KEY_F10){cue=3;return true;}
    return false;
}
static void snapshot(const char *stage) {
    bk_proc_info_t info[128];unsigned ids[128],unique=0,count=bk_proc_snapshot(info,128);
    char args[1024];kstrncpy(args,stage,sizeof(args));args[sizeof(args)-1]=0;
    for(unsigned i=0;i<count;i++) {
        unsigned pid=info[i].process_id?info[i].process_id:info[i].pid;bool seen=false;
        for(unsigned j=0;j<unique;j++)if(ids[j]==pid)seen=true;
        if(seen)continue;ids[unique++]=pid;
        kprintf("[WINEMEM:PROC] stage=%s pid=%u name=%s heap_hint=%u\n",stage,pid,info[i].name,info[i].memory_bytes);
        char word[16];snprintf(word,sizeof(word)," %u",pid);
        if(kstrlen(args)+kstrlen(word)+1<sizeof(args))kstrcat(args,word);
    }
    int status=bk_app_run_sync("/SYSTEM/TEST/MEMPROBE.ELF",args);
    kprintf("[WINEMEM:SAMPLE] stage=%s status=%d\n",stage,status);
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    bk_sys_sleep_ms(4000);
    gui_window_t *w=bk_gui_create_window(desktop,0,0,180,80,"Wine memory");
    if(!w){kprintf("[WINEMEM] FAIL monitor\n");return;}
    bk_gui_set_window_event_handler(w,event,NULL);
    snapshot("baseline");
    bool launched=bk_app_launch("/WINEGATE/bin/wine-memory.ELF",WINEMEM_TARGET);
    kprintf("[WINEMEM:LAUNCH] result=%u\n",launched);
    for(unsigned tick=0;tick<18000 && !bk_proc_exit_requested();tick++) {
        bk_sys_sleep_ms(100);
        if(cue==1){cue=0;snapshot("open");}
        if(cue==2){cue=0;snapshot("functional");}
        if(cue==3){cue=0;snapshot("closed");kprintf("[WINEMEM] DONE\n");return;}
        if(tick%300U==299U)snapshot("startup");
    }
    kprintf("[WINEMEM] TIMEOUT\n");
}
