#!/usr/bin/env python3
"""Exercise the production selector, including icons and changing windows."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define kprintf(...) ((void)0)
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;int width,height;} gui_image_t;
typedef struct {int width,height;} gui_surface_t;
typedef struct window {bool listed,focused,minimized;uint32_t id,owner_pid,controller_pid;uint32_t *icon_pixels;int icon_width,icon_height;char *title;struct window *next;} gui_window_t;
typedef struct {gui_surface_t surface;gui_window_t *first_window;} gui_desktop_t;
typedef struct {bool menu_open,switcher_open,switcher_hover,switcher_top_hover,switcher_capture,switcher_pressed_top;uint32_t switcher_selected,switcher_hover_id,switcher_pressed_id;int switcher_scroll,switcher_icon_count,switcher_icon_next;uint32_t switcher_icon_pids[32];gui_image_t switcher_icons[32];gui_image_t switcher_desktop_icon;} deskbar_state_t;
typedef struct {uint32_t pid,process_id;char name[24];} bk_proc_info_t;
typedef struct {int type,x,y,dy,key,button;} gui_event_t;
enum {GUI_EVENT_MOUSE_MOVE,GUI_EVENT_MOUSE_DOWN,GUI_EVENT_MOUSE_UP,GUI_EVENT_MOUSE_WHEEL,GUI_EVENT_KEY};
#define MOUSE_LEFT_BUTTON 1
#define DESKBAR_LAUNCHER_COUNT 10
#define CDE_FACE_HOVER 0x88868d
#define CDE_PAPER 0xf0efe7
#define CDE_INK 0x191d1c
static const struct {const char *program_path,*icon_name;} g_cde_launchers[10]={
 {"/SYSTEM/PROGRAMS/FILE.BEX","WindowsExplorer"},{"/SYSTEM/PROGRAMS/PAINT.BEX","Mspaint"},
 {"/dummy","unknown"},{"/dummy","unknown"},{"/dummy","unknown"},{"/dummy","unknown"},{"/dummy","unknown"},{"/dummy","unknown"},{"/dummy","unknown"},{"/SYSTEM/CONTROL/CONTROL.BEX","Settings"}};
static int loads,frees,snapshots,focused;
static char loaded_resource[64];static uint32_t pixels[1024];
static uint32_t bk_proc_snapshot(bk_proc_info_t *p,uint32_t n){assert(n>=4);snapshots++;p[0]=(bk_proc_info_t){.pid=5,.process_id=20,.name="GUI callback"};p[1]=(bk_proc_info_t){.pid=10,.process_id=10,.name="FILE.BEX"};p[2]=(bk_proc_info_t){.pid=20,.process_id=20,.name="PAINT.BEX"};p[3]=(bk_proc_info_t){.pid=31,.process_id=30,.name="paint"};return 4;}
static bool bk_graphics_icon_load_size(const char*r,int size,gui_image_t*i){loads++;strcpy(loaded_resource,r);*i=(gui_image_t){pixels,size,size};return true;}
static void bk_gui_image_free(gui_image_t *i){assert(i->pixels);frees++;i->pixels=0;}
static bool gui_rect_contains(gui_rect_t r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static gui_rect_t deskbar_design_rect(const gui_desktop_t*d,int sx,int sy,int sw,int sh){int w=d->surface.width<620?d->surface.width:620,x=(d->surface.width-w)/2,y=d->surface.height-52;int x0=x+(sx*w+967/2)/967,y0=y+(sy*52+85/2)/85;return(gui_rect_t){x0,y0,(sw*w+967/2)/967,(sh*52+85/2)/85};}
static gui_rect_t deskbar_cde_launcher_rect(const gui_desktop_t*d,int i){assert(i==9);return deskbar_design_rect(d,881,21,59,58);}
static gui_rect_t deskbar_menu_rect(const gui_desktop_t*d){(void)d;return(gui_rect_t){260,239,260,310};}
static void deskbar_draw_panel(gui_surface_t*s,gui_rect_t r,uint32_t c,bool b){(void)s;(void)r;(void)c;(void)b;}
#define deskbar_cde_draw_panel deskbar_draw_panel
static void gui_gfx_fill_rect(gui_surface_t*s,gui_rect_t r,uint32_t c){(void)s;(void)r;(void)c;}
static void gui_gfx_draw_line(gui_surface_t*s,int a,int b,int c,int d,uint32_t e){(void)s;(void)a;(void)b;(void)c;(void)d;(void)e;}
static void bk_gui_surface_draw_image(gui_surface_t*s,gui_rect_t a,gui_rect_t b,gui_image_t*i){(void)s;(void)a;(void)b;(void)i;}
static void gui_font_draw_string_clipped(gui_surface_t*s,int x,int y,const char*t,uint32_t c,gui_rect_t r){(void)s;(void)x;(void)y;(void)t;(void)c;(void)r;}
static void deskbar_invalidate(gui_desktop_t*d,deskbar_state_t*s){(void)d;(void)s;}
static void deskbar_reset_menu_state(deskbar_state_t*s){(void)s;}
static void gui_desktop_invalidate_rect(gui_desktop_t*d,gui_rect_t r){(void)d;(void)r;}
static void gui_request_paint(void){}
static void deskbar_focus_window(gui_desktop_t*d,gui_window_t*w){for(gui_window_t*p=d->first_window;p;p=p->next)p->focused=false;w->minimized=false;w->focused=true;focused=w->id;}
#include "system/desktop/deskbar_switcher.h"
static void click(gui_desktop_t*d,deskbar_state_t*s,gui_rect_t r){gui_event_t e={.type=GUI_EVENT_MOUSE_DOWN,.x=r.x+r.w/2,.y=r.y+r.h/2,.button=1};assert(deskbar_switcher_event(d,s,&e));e.type=GUI_EVENT_MOUSE_UP;assert(deskbar_switcher_event(d,s,&e));}
int main(void){
 assert(!strcmp(deskbar_switcher_resource("processmgr"),"Taskman100"));
 gui_window_t w[80]={0};gui_desktop_t d={.surface={800,600}};deskbar_state_t s={0};
 assert(!deskbar_switcher_selected(&d,&s));assert(!deskbar_switcher_layout(&d).count);
 deskbar_switcher_draw(&d,&d.surface,&s);assert(loads==1&&!strcmp(loaded_resource,"KDE1/base/kpanel/pics/window_list"));
 for(int i=0;i<80;i++){w[i].id=i+1;w[i].listed=true;w[i].minimized=true;w[i].title="Window";w[i].next=i?&w[i-1]:0;w[i].owner_pid=i+10;}
 d.first_window=&w[0];click(&d,&s,deskbar_switcher_layout(&d).cell);assert(focused==1&&!w[0].minimized);
 assert(deskbar_switcher_selected(&d,&s)==&w[0]);
 deskbar_switcher_icon(&s,&w[0]);assert(!strcmp(loaded_resource,"WindowsExplorer"));int before=loads;
 deskbar_switcher_icon(&s,&w[0]);assert(loads==before && snapshots==1);
 w[1].owner_pid=20;deskbar_switcher_icon(&s,&w[1]);assert(!strcmp(loaded_resource,"Mspaint"));
 w[2].owner_pid=30;deskbar_switcher_icon(&s,&w[2]);assert(!strcmp(loaded_resource,"Mspaint"));
 w[1].icon_pixels=pixels;w[1].icon_width=w[1].icon_height=16;
 assert(deskbar_switcher_icon(&s,&w[1]).width==16 && loads==before+2);w[1].icon_pixels=0;
 d.first_window=&w[1];deskbar_switcher_layout_t l=deskbar_switcher_layout(&d);assert(l.count==2&&l.rows==2);
 click(&d,&s,l.top);assert(s.switcher_open);click(&d,&s,deskbar_switcher_item(l,1));assert(focused==2&&!s.switcher_open);
 w[1].minimized=true;w[1].focused=false;assert(deskbar_switcher_selected(&d,&s)==&w[1]);click(&d,&s,l.cell);assert(focused==2&&!w[1].minimized);
 click(&d,&s,l.top);gui_event_t e={.type=GUI_EVENT_KEY,.key=27};assert(deskbar_switcher_event(&d,&s,&e)&&!s.switcher_open);
 click(&d,&s,l.top);e=(gui_event_t){.type=GUI_EVENT_MOUSE_DOWN,.x=0,.y=0,.button=1};assert(!deskbar_switcher_event(&d,&s,&e)&&!s.switcher_open);
 // Stable IDs and every entry remain reachable when focus reorders windows.
 d.first_window=&w[79];l=deskbar_switcher_layout(&d);assert(l.scroll_h&&l.menu.y>=0);
 s.switcher_scroll=999;click(&d,&s,l.top);s.switcher_scroll=l.count-l.rows;
 click(&d,&s,deskbar_switcher_item(l,l.rows-1));assert(focused==80);

 // Closing the pressed window must not activate a different entry.
 click(&d,&s,l.top);s.switcher_scroll=0;gui_rect_t r=deskbar_switcher_item(l,0);
 e=(gui_event_t){.type=GUI_EVENT_MOUSE_DOWN,.x=r.x+2,.y=r.y+2,.button=1};assert(deskbar_switcher_event(&d,&s,&e));
 w[0].listed=false;focused=0;e.type=GUI_EVENT_MOUSE_UP;assert(deskbar_switcher_event(&d,&s,&e));assert(!focused);w[0].listed=true;
 for(int width=320;width<=1280;width+=160)for(int height=200;height<=960;height+=120){d.surface=(gui_surface_t){width,height};l=deskbar_switcher_layout(&d);assert(l.menu.x==deskbar_design_rect(&d,646,4,294,17).x&&l.menu.y>=0&&l.menu.x+l.menu.w<=width&&l.menu.y+l.menu.h<l.top.y);}
 // Cache is bounded and releases evicted artwork; unknown programs get E8.
 for(int i=2;i<80;i++){w[i].owner_pid=100+i;deskbar_switcher_icon(&s,&w[i]);}
 assert(s.switcher_icon_count==32&&frees>=48&&!strcmp(loaded_resource,"ExecutableWindow"));
 d.first_window=0;s.switcher_open=true;e=(gui_event_t){.type=GUI_EVENT_MOUSE_MOVE};deskbar_switcher_event(&d,&s,&e);assert(!s.switcher_open&&!s.switcher_selected);
 puts("PASS: empty/one/many windows, real executable icons and bounded cache, focus/restore, stable menu, scroll-to-last, Escape, outside click and close during press");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-switcher-') as tmp:
 c=Path(tmp)/'test.c';binary=Path(tmp)/'test';c.write_text(code)
 subprocess.run(['gcc','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root),str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
