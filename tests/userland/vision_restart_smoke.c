#include "../../kernel/include/api.h"
static bool failed;
static volatile uint32_t paints;
static volatile uint32_t dialog_cancelled;
static void dialog_result(const char *path,void *context) {
    if(!path && context==(void *)0x1234U)dialog_cancelled++;
}
static void check(bool ok,const char *name) {
    kprintf("[VISIONTEST] %s: %s\n",name,ok?"PASS":"FAIL");
    if(!ok)failed=true;
}
static void paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    (void)context;gui_rect_t bounds;
    if(bk_gui_window_content_rect(window,&bounds)) {
        bk_gui_surface_fill_rect(surface,bounds,0x00204060U);
        bk_gui_surface_draw_text(surface,bounds.x+12,bounds.y+12,"Cliente sigue vivo",0x00ffffffU,0U,false);
    }
    ++paints;
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    bk_sys_sleep_ms(7000);
    gui_window_t *window=bk_gui_create_window(desktop,140,90,300,170,"Reinicio de Vision");
    check(window!=NULL,"create surviving client window");
    if(!window){kprintf("[VISIONTEST] DONE result=FAIL\n");return;}
    bk_gui_set_window_content(window,paint,NULL);
    bk_sys_sleep_ms(2000);
    bk_memory_detail_t before,after;bk_memory_detail(&before);
    for(uint32_t cycle=0;cycle<3;cycle++) {
        bk_startup_info_t old,current;bool restarted=false;
        check(bk_startup_get(0,&old) && old.pid,"find Vision service");
        check(!bk_service_control(1,BK_SERVICE_STOP),"critical stop remains protected");
        if(cycle==0) {
            check(bk_file_dialog_open(desktop,"Pending during restart","/SYSTEM",NULL,0,dialog_result,(void *)0x1234U),"open pending dialog before server exit");
            bk_sys_sleep_ms(2000);
        }
#if VISION_EXPECT_FAULT
        if(cycle==0)
            check(bk_file_write_all("/SYSTEM/USER/CONFIG/VISION.FAULT","X",1),"arm fatal server exception");
        else
#endif
            check(bk_service_control(1,BK_SERVICE_RESTART),"request controlled restart");
        for(uint32_t wait=0;wait<600;wait++) {
            bk_sys_sleep_ms(100);
            if(bk_startup_get(0,&current) && current.pid && current.pid!=old.pid &&
               current.state==BK_SERVICE_RUNNING){restarted=true;break;}
        }
        check(restarted,"new server becomes READY");
        if(cycle==0) {
            for(uint32_t wait=0;wait<100U && !dialog_cancelled;wait++)bk_sys_sleep_ms(100);
            check(dialog_cancelled==1U,"pending dialog cancelled exactly once after server exit");
        }
        check(bk_gui_window_is_open(window),"client window survives restart");
        uint32_t previous=paints;
        check(bk_gui_window_set_bounds(window,(gui_rect_t){140+(int)cycle*12,90,320+(int)cycle*8,180}),"resize after restart");
        bk_gui_window_invalidate(window);
        uint32_t callback_started=bk_sys_uptime_ms();
        for(uint32_t wait=0;wait<100U && paints==previous;wait++)bk_sys_sleep_ms(100);
        kprintf("[VISIONTEST] callback latency_ms=%u\n",bk_sys_uptime_ms()-callback_started);
        check(paints>previous,"client paint callback recovers");
        if(paints==previous)
            kprintf("[VISIONTEST:STATE] dirty=%u pending=%u ready=%u repaint=%u visible=%u slot=%d\n",window->dirty,window->content_pending,window->content_ready,window->content_repaint,window->visible,window->content_staging_slot);
        kprintf("[VISIONTEST] cycle=%u old=%u new=%u paints=%u\n",cycle,old.pid,current.pid,paints);
        bk_sys_sleep_ms(1500);
    }
    bk_memory_detail(&after);
    kprintf("[VISIONTEST] heap before=%u after=%u\n",before.heap_used,after.heap_used);
    check(after.heap_used<=before.heap_used+128U*1024U,"server resources reclaimed over three restarts");
    bk_gui_close_window(window);
    kprintf("[VISIONTEST] DONE result=%s\n",failed?"FAIL":"PASS");
}
