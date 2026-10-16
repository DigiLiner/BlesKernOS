#!/usr/bin/env python3
"""Exercise production mouse panel handlers with two open instances."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'system/control/mouse.c').read_text()
def function(name):
 pos=s.index(name+'(');start=s.rfind('static ',0,pos);begin=s.index('{',pos);depth=1;end=begin+1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
struct=s[s.index('typedef struct {'):s.index('static const cpl_tab_spec_t')]
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define MOUSE_LEFT_BUTTON 1
#define MOUSE_RIGHT_BUTTON 2
#define MOUSE_MIDDLE_BUTTON 4
#define MOUSE_TAB_POINTER 0
#define MOUSE_TAB_OPTIONS 1
#define MOUSE_TAB_BUTTONS 2
#define MOUSE_TAB_GENERAL 3
#define MOUSE_CPL_TAB_COUNT 4
#define MOUSE_CPL_TAB_LEFT 12
#define MOUSE_CPL_TAB_TOP 8
#define BK_INPUT_SWAP_BUTTONS 4
#define BK_INPUT_DOUBLE_CLICK 5
#define BK_INPUT_CURSOR_SCHEME 6
#define BK_MOUSE_CONFIG_PATH "/MOUSE.INI"
enum {GUI_EVENT_MOUSE_DOWN,GUI_EVENT_KEY};
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {bool present;} mouse_state_t;
typedef struct {bool visible,selected,pressed;int value;uint32_t id;} gui_widget_t;
typedef struct {bool trail;} gui_desktop_t;
typedef struct {void *content_context;bool dirty;gui_rect_t client;} gui_window_t;
typedef struct {int type,x,y,button;char key;} gui_event_t;
static int global_speed=3;static uint32_t now;static bool disk_ok=true;static char saved[256];static int g_mouse_tabs[2];
static gui_rect_t bk_gui_window_content_rect_raw(gui_window_t*w){return w->client;}
static uint32_t prefs[]={500,60,500,0,0,500,0};
static uint32_t bk_input_preference_get(uint32_t k){return prefs[k];}
static int bk_input_mouse_get_sensitivity(void){return global_speed;}
static void bk_input_mouse_set_sensitivity(uint8_t speed){global_speed=speed;}
static bool bk_gui_desktop_cursor_trail_enabled(gui_desktop_t*d){return d->trail;}
static void bk_gui_desktop_set_cursor_trail(gui_desktop_t*d,bool on){d->trail=on;}
static void bk_gui_choice_set_checked(gui_widget_t*w,bool selected){if(w)w->selected=selected;}
static void bk_gui_slider_set_value(gui_widget_t*w,int value){if(w)w->value=value;}
static int bk_gui_slider_get_value(gui_widget_t*w){return w->value;}
static void bk_runtime_strncpy(char*d,const char*s,int size){strncpy(d,s,size);}
static bool bk_user_config_write_text(const char*p,const char*t){(void)p;strcpy(saved,t);return disk_ok;}
static uint32_t bk_sys_uptime_ms(void){return now;}
static bool bk_gui_rect_contains(gui_rect_t r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static gui_rect_t cpl_inset(gui_rect_t r,int n){return (gui_rect_t){r.x+n,r.y+n,r.w-2*n,r.h-2*n};}
static int cpl_hit_tab(gui_rect_t c,const int*t,int n,int l,int top,int x,int y){(void)t;(void)n;for(int i=0;i<2;i++)if(bk_gui_rect_contains((gui_rect_t){c.x+l+72*i,c.y+top,72,24},x,y))return i;return -1;}
'''+struct
for name in ['mouse_cpl_client_rect','mouse_cpl_page_rect','mouse_cpl_preview_group_rect','mouse_cpl_preview_canvas_rect','mouse_cpl_default_status','mouse_cpl_set_status','mouse_cpl_sync_buttons','mouse_cpl_sync_widget_visibility','mouse_cpl_save_config','mouse_cpl_sync_system_state','mouse_cpl_reset_test','mouse_cpl_switch_tab','mouse_cpl_sensitivity','mouse_cpl_toggle_trail','mouse_cpl_event']:code+=function(name)
code+=r'''
int main(void){
 gui_desktop_t d={0};gui_window_t wa={.client={100,100,456,350}},wb={.client={200,150,456,350}};
 mouse_cpl_state_t a={.desktop=&d,.window=&wa},b={.desktop=&d,.window=&wb};wa.content_context=&a;wb.content_context=&b;
 gui_widget_t speed={.id=10,.value=3},trail={.id=20};
 a.sensitivity_slider=b.sensitivity_slider=&speed;a.trail_checkbox=b.trail_checkbox=&trail;
 assert(mouse_cpl_sync_system_state(&a));assert(mouse_cpl_sync_system_state(&b));
 speed.value=5;mouse_cpl_sensitivity(&wa,10);assert(global_speed==5);assert(!d.trail);
 /* b still holds speed 3. Toggling trail must preserve the newer global speed. */
 trail.selected=true;mouse_cpl_toggle_trail(&wb,20);assert(d.trail&&global_speed==5);
 assert(strstr(saved,"sensitivity=5")&&strstr(saved,"trail=1"));
 assert(mouse_cpl_sync_system_state(&a)&&a.draw_trail);assert(!mouse_cpl_sync_system_state(&a));
 disk_ok=false;speed.value=1;mouse_cpl_sensitivity(&wa,10);assert(global_speed==1&&d.trail);assert(strstr(a.status,"no se pudo guardar"));
 gui_rect_t r=mouse_cpl_preview_canvas_rect(&a);gui_event_t e={.type=GUI_EVENT_MOUSE_DOWN,.x=r.x+20,.y=r.y+20,.button=1};
 now=100;assert(mouse_cpl_event(&wa,&e,&a));now=350;assert(mouse_cpl_event(&wa,&e,&a));assert(a.clicks[0]==2&&a.double_clicks==1);
 now=400;assert(mouse_cpl_event(&wa,&e,&a));assert(a.double_clicks==1); /* Three clicks make one pair. */
 now=1100;assert(mouse_cpl_event(&wa,&e,&a));assert(a.double_clicks==1);
 e.button=2;assert(mouse_cpl_event(&wa,&e,&a));e.button=4;assert(mouse_cpl_event(&wa,&e,&a));assert(a.clicks[1]==1&&a.clicks[2]==1);
 e.x=r.x-1;assert(!mouse_cpl_event(&wa,&e,&a));assert(a.clicks[2]==1);
 mouse_cpl_switch_tab(&a,1);e.x=r.x+20;e.button=1;assert(!mouse_cpl_event(&wa,&e,&a));assert(a.clicks[0]==4);
 mouse_cpl_switch_tab(&a,0);mouse_cpl_reset_test(&wa,0);assert(a.clicks[0]==0&&a.double_clicks==0&&!a.click_visible);
 puts("PASS mouse: live global settings, stale-panel updates, persistence failure, all buttons, double-click pairs and test bounds");
}
'''
# Run the actual generic mouse preference functions against driver replacement.
core=(ROOT/'kernel/drivers/input/mouse_core.c').read_text()
def core_function(name):
 pos=core.index(name+'(');start=core.rfind('\n',0,pos)+1;begin=core.index('{',pos);depth=1;end=begin+1
 while depth:
  depth+=(core[end]=='{')-(core[end]=='}');end+=1
 return core[start:end]+'\n'
code2=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef struct {bool (*is_present)(void);void (*get_state)(void*);void (*set_bounds)(int,int);void (*set_position)(int,int);void (*set_sensitivity)(uint8_t);uint8_t (*get_sensitivity)(void);} mouse_driver_ops_t;
static const mouse_driver_ops_t *g_mouse_driver;static uint8_t g_fallback_sensitivity=3,device_speed;
static bool present(void){return true;}static void state(void*s){(void)s;}static void pair(int a,int b){(void)a;(void)b;}
static void set(uint8_t s){device_speed=s;}static uint8_t get(void){return device_speed;}
'''
for name in ['mouse_register_driver','mouse_set_sensitivity','mouse_get_sensitivity']:code2+=core_function(name)
code2+=r'''
int main(void){mouse_driver_ops_t ops={present,state,pair,pair,set,get};
 mouse_set_sensitivity(5);assert(mouse_get_sensitivity()==5);assert(mouse_register_driver(&ops));assert(device_speed==5);
 device_speed=2;assert(mouse_get_sensitivity()==5);assert(mouse_register_driver(&ops));assert(device_speed==5);
 mouse_set_sensitivity(0);assert(device_speed==1&&mouse_get_sensitivity()==1);mouse_set_sensitivity(255);assert(device_speed==5);
 assert(!mouse_register_driver(NULL));puts("PASS mouse core: one authoritative sensitivity, driver replacement and bounds");}
'''
with tempfile.TemporaryDirectory(prefix='bk-mouse-core-') as folder:
 p=Path(folder);(p/'test.c').write_text(code2)
 subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
with tempfile.TemporaryDirectory(prefix='bk-mouse-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
