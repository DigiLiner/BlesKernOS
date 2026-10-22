#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
#include "../../kernel/include/keyboard.h"
#ifndef WINE_UI_QUIET
#define WINE_UI_QUIET 0
#endif
#ifndef WINE_UI_LOAD_PROBE
#define WINE_UI_LOAD_PROBE 0
#endif
static volatile bool restart_requested,restart_accepted;
static volatile uint32_t paints;
static volatile bool check_before,check_after;
static uint32_t load_color;
static bool event(gui_window_t *window,const gui_event_t *e,void *context) {
    (void)window;(void)context;
    if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F7) {
        ++load_color;bk_gui_window_invalidate(window);return true;
    }
    if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F10 && !restart_requested) {
        restart_requested=true;restart_accepted=bk_service_control(1,BK_SERVICE_RESTART);
        kprintf("[WINEUITEST] restart requested result=%u\n",restart_accepted);return true;
    }
    if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F9){check_before=true;return true;}
    if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F8){check_after=true;return true;}
    return false;
}
static void paint(gui_window_t *window,gui_surface_t *surface,void *context) {
    (void)context;gui_rect_t rect;
    if(bk_gui_window_content_rect(window,&rect)) {
        bk_gui_surface_fill_rect(surface,rect,0x00204060U+(load_color&127U)*0x00010101U);
        bk_gui_surface_draw_text(surface,rect.x+8,rect.y+8,"F10: reiniciar Vision",0x00ffffffU,0,false);
    }
    paints++;
}
void bleskernos_program_main(gui_desktop_t *desktop) {
    bk_sys_sleep_ms(4000);
    gui_window_t *window=bk_gui_create_window(desktop,600,100,300,170,"Wine y Vision");
    if(!window){kprintf("[WINEUITEST] DONE result=FAIL create\n");return;}
    bk_gui_set_window_content(window,paint,NULL);bk_gui_set_window_event_handler(window,event,NULL);
#if WINE_UI_LOAD_PROBE
    for(uint32_t n=0;!paints && n<100U;n++)bk_sys_sleep_ms(20U);
    if(!paints){kprintf("[WINEUITEST] DONE result=FAIL native paint not ready\n");return;}
#endif
    bk_startup_info_t original,current;bk_startup_get(0,&original);
    bool launched=bk_app_launch("/SYSTEM/PROGRAMS/WINE.BEX","/SYSTEM/WINE/WINDOWS/SYSTEM32/NOTEPAD.EXE");
    kprintf("[WINEUITEST] Notepad launch=%u\n",launched);
    bool clipboard=false,clipboard_after=false,restarted=false;uint32_t paints_at_restart=0;
    for(uint32_t wait=0;wait<6000U;wait++) {
        bk_sys_sleep_ms(100);
#if !WINE_UI_QUIET
        if(wait%200U==0U) {bk_proc_info_t processes[24];uint32_t count=bk_proc_snapshot(processes,24);
          for(uint32_t n=0;n<count;n++)kprintf("[WINEUITEST:PROC] pid=%u process=%u name=%s state=%u cpu=%u exit=%u\n",processes[n].pid,processes[n].process_id,processes[n].name,processes[n].state,processes[n].cpu_ticks,processes[n].exit_requested);
        }
#endif
        /* F9/F8 are sent only after the host compares the actual Notepad
         * pixels produced by copy/paste and keyboard input. */
        if(check_before && !clipboard) {
            check_before=false;clipboard=true;
            kprintf("[WINEUITEST] Notepad keyboard and clipboard verified\n");
        }
        if(check_after && restarted){check_after=false;clipboard_after=true;}
        if(restart_requested && !restarted && bk_startup_get(0,&current) &&
           current.pid && current.pid!=original.pid && current.state==BK_SERVICE_RUNNING) {
            restarted=true;paints_at_restart=paints;bk_gui_window_invalidate(window);
            kprintf("[WINEUITEST] replacement ready pid=%u\n",current.pid);
        }
        if(launched && clipboard && restarted && restart_accepted &&
           paints>paints_at_restart && bk_gui_window_is_open(window) &&
           clipboard_after) {
            kprintf("[WINEUITEST] DONE result=PASS Notepad input/clipboard after restart\n");
            while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);return;
        }
    }
    kprintf("[WINEUITEST] DONE result=FAIL launched=%u clipboard=%u restarted=%u paints=%u\n",launched,clipboard,restarted,paints);
}
