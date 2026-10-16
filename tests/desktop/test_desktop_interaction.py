#!/usr/bin/env python3
"""Regression checks on the actual shell helpers, ghost renderer and drop protocol."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
desk=(root/'system/desktop/deskmanager.c').read_text()
api=(root/'kernel/api/api.c').read_text()
comp=(root/'gui/compositor.c').read_text()
def function(source,signature):
 start=source.index(signature);brace=source.index('{',start);depth=1;i=brace+1
 while depth:
  if source[i]=='{':depth+=1
  elif source[i]=='}':depth-=1
  i+=1
 return source[start:i]+'\n'
functions=''.join(function(desk,x) for x in ['static gui_rect_t desk_icon_rect(', 'static void desk_select_area(',
 'static void desk_arrange_icons(deskmanager_state_t *st,gui_desktop_t *desktop) {','static bool desk_resize_icons(',
 'static void desk_icon_ghost(','static gui_rect_t desk_context_menu_rect('])
start=desk.index('enum {\n    DESK_CONTEXT_OPEN');end=desk.index('};',start)+2
functions+=desk[start:end]+'\n'
functions+=function(desk,'static void desk_open_desktop_menu(')+function(desk,'static bool desk_context_event(')
functions+=function(desk,'static bool desk_queue_files_drop(')
functions+=function(api,'bool bk_gui_file_drag_start_icon(')
functions+=function(api,'bool bk_gui_file_drop_take(')
functions+=function(comp,'static void compositor_draw_icon_ghost(')
shim=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define DESK_MAX_ICONS 128
#define GUI_EVENT_MOUSE_MOVE 1
#define GUI_EVENT_MOUSE_DOWN 2
#define GUI_EVENT_MOUSE_UP 3
#define GUI_EVENT_KEY 5
#define GUI_EVENT_FILE_DROP 11
#define KEY_ESCAPE 27
#define MOUSE_LEFT_BUTTON 1
static int g_desktop_icon_size=48;
#define DESK_ICON_W (g_desktop_icon_size+10)
#define DESK_ICON_H (g_desktop_icon_size+28)
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {uint32_t *pixels;int width,height,pitch;gui_rect_t clip;} gui_surface_t;
typedef struct {int type,x,y,dx,dy,button,key;} gui_event_t;
typedef struct {unsigned owner_pid;bool listed;} gui_window_t;
typedef struct {unsigned id;char label[24];} menu_item;
typedef struct {int x,y,width,item_count,pressed_item;bool open;menu_item items[16];} gui_context_menu_t;
typedef struct {
 gui_surface_t surface;
 unsigned mouse_buttons;char file_drag_path[260];gui_window_t *file_drag_window;bool file_drag_active;
 struct {uint32_t used,token,tick;char path[260];} file_drops[4];
 char (*file_drop_batches[4])[260];uint32_t file_drop_counts[4],file_drop_positions[4];gui_window_t *file_drop_windows[4];
 uint32_t file_drop_sequence;bool drag_icon_ghost;uint8_t drag_icon_size,drag_icon_count,drag_icon_mask[4096];gui_rect_t drag_outline_bounds;
} gui_desktop_t;
typedef struct {int x,y;bool hidden,selected;uint32_t *pixels,*original_pixels;uint16_t original_width,original_height;} desk_icon_t;
typedef struct {
 desk_icon_t icons[128];int icon_count,selected_icon;bool selection_before[128],auto_arrange,align_grid;
 gui_context_menu_t context_menu,desktop_menu;unsigned context_pressed;
} deskmanager_state_t;
static gui_desktop_t desktop;
static gui_event_t queued;
static bool queue_ok=true;
static unsigned now,callback_id;
static int allocation_fail=-1;
static void *bk_sys_alloc_zero(unsigned n){if(allocation_fail==0)return NULL;if(allocation_fail>0)--allocation_fail;return calloc(1,n);}
#define bk_sys_free free
#define kfree free
#define kmemcpy memcpy
#define kmemset memset
#define kstrncpy strncpy
#define kstrcpy strcpy
#define kstrlen strlen
#define bk_runtime_strncpy strncpy
static uint32_t bk_sys_ticks(void){return now;}
static uint32_t desk_ticks_from_ms(unsigned n){return n/10;}
static bool gui_window_dispatch_event(gui_window_t *w,const gui_event_t *e){(void)w;queued=*e;return queue_ok;}
static gui_desktop_t *gui_get_desktop(void){return &desktop;}
static unsigned task_current_process_id(void){return 1;}
static bool gui_rect_contains(gui_rect_t r,int x,int y){return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;}
static bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t *r){
 int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
 int ex=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,ey=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
 if(a.w<=0 || a.h<=0 || b.w<=0 || b.h<=0 || ex<=x || ey<=y)return false;
 *r=(gui_rect_t){x,y,ex-x,ey-y};return true;
}
static gui_rect_t gui_gfx_get_clip(gui_surface_t *s){return s->clip;}
static void gui_desktop_invalidate_rect(gui_desktop_t *d,gui_rect_t r){(void)d;(void)r;}
static void gui_desktop_invalidate_all(gui_desktop_t *d){(void)d;}
static void gui_request_paint(void){}
static void desk_context_callback(gui_window_t *w,uint32_t id,void *p){(void)w;(void)p;callback_id=id;}
static void gui_context_menu_clear(gui_context_menu_t *m){memset(m,0,sizeof(*m));m->width=180;m->pressed_item=-1;}
static void gui_context_menu_close(gui_context_menu_t *m){m->open=false;}
static bool gui_context_menu_add_item(gui_context_menu_t *m,unsigned id,const char *label,bool enabled,void (*cb)(gui_window_t*,uint32_t,void*),void *context){
 (void)enabled;(void)cb;(void)context;m->items[m->item_count++].id=id;strncpy(m->items[m->item_count-1].label,label,23);return true;
}
static void gui_context_menu_open(gui_context_menu_t *m,int x,int y,gui_rect_t r){
 int h=m->item_count*22+4;if(x+m->width>r.w)x=r.w-m->width;if(y+h>r.h)y=r.h-h;
 if(x<0)x=0;
 if(y<0)y=0;
 m->x=x;m->y=y;m->open=true;
}
'''
test=r'''
int main(void){
 desktop.surface.width=800;desktop.surface.height=600;
 deskmanager_state_t st={0};st.icon_count=5;st.icons[1].hidden=true;
 desk_arrange_icons(&st,&desktop);
 assert(st.icons[0].x==12 && st.icons[0].y==12);assert(st.icons[2].y==96);
 for(int i=0;i<5;++i)if(!st.icons[i].hidden)for(int j=i+1;j<5;++j)if(!st.icons[j].hidden){gui_rect_t overlap;assert(!gui_rect_intersect(desk_icon_rect(&st.icons[i]),desk_icon_rect(&st.icons[j]),&overlap));}
 desk_select_area(&st,&desktop,(gui_rect_t){0,0,90,185});
 assert(st.icons[0].selected && !st.icons[1].selected && st.icons[2].selected && !st.icons[4].selected);
 desk_select_area(&st,&desktop,(gui_rect_t){0,0,1,1});assert(!st.icons[0].selected);
 st.selection_before[4]=true;desk_select_area(&st,&desktop,(gui_rect_t){0,0,90,80});assert(st.icons[0].selected && st.icons[4].selected);
 for(int i=0;i<5;++i){
  st.icons[i].original_width=64;st.icons[i].original_height=64;
  st.icons[i].original_pixels=calloc(64*64,4);st.icons[i].pixels=calloc(48*48,4);
  for(int y=0;y<64;++y)for(int x=0;x<64;++x)st.icons[i].original_pixels[y*64+x]=0xff123456u+y*64+x;
  for(int y=0;y<48;++y)for(int x=0;x<48;++x)st.icons[i].pixels[y*48+x]=st.icons[i].original_pixels[(y*64/48)*64+x*64/48];
 }
 allocation_fail=1;uint32_t *original=st.icons[0].pixels;
 assert(!desk_resize_icons(&st,64));assert(g_desktop_icon_size==48 && st.icons[0].pixels==original);
 allocation_fail=-1;assert(desk_resize_icons(&st,32));assert(g_desktop_icon_size==32 && st.icons[0].pixels[0]==0xff123456);
 for(int cycle=0;cycle<8;++cycle){
  assert(desk_resize_icons(&st,64));assert(desk_resize_icons(&st,32));assert(desk_resize_icons(&st,48));
  for(int y=0;y<48;++y)for(int x=0;x<48;++x)assert(st.icons[0].pixels[y*48+x]==st.icons[0].original_pixels[(y*64/48)*64+x*64/48]);
 }
 for(int i=0;i<5;++i){free(st.icons[i].pixels);free(st.icons[i].original_pixels);}
 uint32_t icon[64*64]={0},screen[80*80];for(int y=10;y<22;++y)for(int x=10;x<22;++x)icon[y*32+x]=0xff112233;
 desk_icon_ghost(&desktop,icon,32,1);assert(!desktop.drag_icon_mask[0] && desktop.drag_icon_mask[10*64+10]==2 && desktop.drag_icon_mask[12*64+12]==1);
 for(int i=0;i<80*80;++i)screen[i]=0x00556677;
 desktop.drag_outline_bounds=(gui_rect_t){5,5,32,32};gui_surface_t surface={screen,80,80,80,{0,0,80,80}};
 compositor_draw_icon_ghost(&desktop,&surface);assert(screen[5*80+5]==0x00556677 && screen[15*80+15]!=0x00556677);
 for(int i=0;i<80*80;++i)assert(screen[i]!=0x00112233); /* Colors never appear in the ghost. */
 icon[0]=0x01112233;desk_icon_ghost(&desktop,icon,32,1);assert(desktop.drag_icon_mask[0]==2);
 gui_window_t drag_source={.owner_pid=1,.listed=true};desktop.mouse_buttons=1;
 uint32_t actual_logo[32*32]={0};actual_logo[9*32+7]=0x01112233;
 assert(bk_gui_file_drag_start_icon(&drag_source,"/logo",actual_logo,32,32));
 assert(desktop.file_drag_active && desktop.drag_icon_ghost && desktop.drag_icon_size==32);
 assert(desktop.drag_icon_mask[9*64+7]==2 && !desktop.drag_icon_mask[9*64+8]);
 actual_logo[9*32+7]=0;assert(desktop.drag_icon_mask[9*64+7]==2); /* Copied, never borrowed. */
 drag_source.owner_pid=2;assert(!bk_gui_file_drag_start_icon(&drag_source,"/logo",actual_logo,32,32));
 drag_source.owner_pid=1;desktop.mouse_buttons=0;assert(!bk_gui_file_drag_start_icon(&drag_source,"/logo",actual_logo,32,32));
 desktop.mouse_buttons=1;assert(!bk_gui_file_drag_start_icon(&drag_source,"/logo",actual_logo,257,32));
 assert(bk_gui_file_drag_start_icon(&drag_source,"/logo",NULL,0,0) && !desktop.drag_icon_ghost);
 st.context_menu=(gui_context_menu_t){.x=620,.y=40,.width=180,.item_count=2,.open=true};st.context_menu.items[0].id=DESK_CONTEXT_DESKTOP;st.context_menu.items[1].id=DESK_CONTEXT_FILES;
 gui_event_t e={.type=GUI_EVENT_MOUSE_MOVE,.x=650,.y=50};assert(desk_context_event(&st,&desktop,&e));assert(st.desktop_menu.open && st.desktop_menu.x<st.context_menu.x);
 e=(gui_event_t){.type=GUI_EVENT_MOUSE_MOVE,.x=st.desktop_menu.x+8,.y=st.desktop_menu.y+10};assert(desk_context_event(&st,&desktop,&e));
 e.type=GUI_EVENT_MOUSE_UP;e.button=1;assert(desk_context_event(&st,&desktop,&e));assert(!callback_id);
 e.type=GUI_EVENT_MOUSE_DOWN;assert(desk_context_event(&st,&desktop,&e));e.type=GUI_EVENT_MOUSE_UP;assert(desk_context_event(&st,&desktop,&e));assert(callback_id==DESK_CONTEXT_AUTO && !st.desktop_menu.open && !st.context_menu.open);
 st.context_menu.open=true;e=(gui_event_t){.type=GUI_EVENT_KEY,.key=KEY_ESCAPE};assert(desk_context_event(&st,&desktop,&e));assert(!st.context_menu.open);
 char paths[7][260],output[260];for(int i=0;i<7;++i)snprintf(paths[i],260,"/SYSTEM/DESKTOP/file%d",i);
 gui_window_t window={.owner_pid=1},other={.owner_pid=1};e=(gui_event_t){.x=200,.y=200};
 assert(desk_queue_files_drop(&desktop,&window,&e,paths,7));gui_event_t drop=queued;
 strcpy(paths[0],"changed");assert(!bk_gui_file_drop_take(&other,&drop,output));
 gui_event_t stale=drop;stale.dy++;assert(!bk_gui_file_drop_take(&window,&stale,output));
 for(int i=0;i<7;++i){assert(bk_gui_file_drop_take(&window,&drop,output));char expected[260];snprintf(expected,260,"/SYSTEM/DESKTOP/file%d",i);assert(!strcmp(output,expected));}
 assert(!bk_gui_file_drop_take(&window,&drop,output));assert(!desktop.file_drop_batches[drop.dx]);
 queue_ok=false;assert(!desk_queue_files_drop(&desktop,&window,&e,paths,7));assert(!desktop.file_drops[drop.dx].used && !desktop.file_drop_batches[drop.dx]);
 puts("PASS desktop grid, selection/additive box, resize rollback, masked ghost, cascading menu, batch drop ownership and lifetime");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-desktop-interaction-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+functions+test)
 subprocess.run(['gcc','-Wall','-Wextra',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
