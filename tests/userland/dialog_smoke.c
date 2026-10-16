#define BK_API_IMPLEMENTATION
#include "../../kernel/include/api.h"
#include "../../kernel/include/about_dialog.h"
#include "../../kernel/include/keyboard.h"
static volatile bool cancelled,restart,finish;
static uint32_t paints;
static void selected(const char *path,void *context){(void)context;cancelled=path==NULL;kprintf("[DIALOGTEST] cancellation callback=%u\n",cancelled);}
static bool event(gui_window_t *w,const gui_event_t *e,void *ctx){(void)w;(void)ctx;
 if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F11){restart=bk_service_control(1,BK_SERVICE_RESTART);return true;}
 if(e->type==GUI_EVENT_KEY && (uint8_t)e->key==KEY_F12){finish=true;return true;}return false;
}
static void paint(gui_window_t *w,gui_surface_t *s,void *ctx){(void)ctx;gui_rect_t r;if(bk_gui_window_content_rect(w,&r)){bk_gui_surface_fill_rect(s,r,0x203040);bk_gui_surface_draw_text(s,r.x+8,r.y+8,"F11 reinicia, F12 verifica",0xffffff,0,false);}paints++;}
void bleskernos_program_main(gui_desktop_t *desktop){
 bk_sys_sleep_ms(3000);gui_window_t *w=bk_gui_create_window(desktop,10,400,300,150,"Dialog monitor");
 if(!w){kprintf("[DIALOGTEST] DONE result=FAIL create\n");return;}
 bk_gui_set_window_content(w,paint,NULL);bk_gui_set_window_event_handler(w,event,NULL);
 bk_about_info_t info={"About Ring3","0.9","Dialog validation","BlesKernOS",""};
 bool attached=bk_about_attach(w,desktop,&info);
 bool opened=bk_file_dialog_open(desktop,"Selector Ring3","/SYSTEM","",0,selected,NULL);
 kprintf("[DIALOGTEST] attached=%u opened=%u\n",attached,opened);
 bool shown=false,replaced=false;bk_startup_info_t before,current;bk_startup_get(0,&before);
 for(uint32_t n=0;n<4000;n++){
  bk_sys_sleep_ms(100);
  if(cancelled && !shown){bk_about_show(desktop,&info);shown=true;kprintf("[DIALOGTEST] About requested\n");}
  if(restart && !replaced && bk_startup_get(0,&current) && current.pid && current.pid!=before.pid && current.state==BK_SERVICE_RUNNING){replaced=true;kprintf("[DIALOGTEST] replacement ready\n");}
  if(finish){kprintf("[DIALOGTEST] DONE result=%s paints=%u\n",attached && opened && cancelled && shown && replaced && paints?"PASS":"FAIL",paints);while(!bk_proc_exit_requested())bk_sys_sleep_ms(100);return;}
 }
 kprintf("[DIALOGTEST] DONE result=FAIL timeout\n");
}
