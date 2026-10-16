#!/usr/bin/env python3
"""Exercise production slider/choice input, validation and notification semantics."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source,name):
 pos=source.index(name+'(');start=source.rfind('\n',0,pos)+1;opening=source.index('{',pos);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
widget=(ROOT/'gui/widget.c').read_text();api=(ROOT/'kernel/api/api.c').read_text()
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define MOUSE_LEFT_BUTTON 1
#define GUI_WIDGET_STYLE_BUTTON 0
#define GUI_WIDGET_STYLE_CHECKBOX 4
#define GUI_WIDGET_STYLE_RADIO 5
#define GUI_WIDGET_STYLE_SLIDER 6
#define GUI_WIDGET_STYLE_LISTBOX 2
#define GUI_EVENT_MOUSE_DOWN 1
#define GUI_EVENT_MOUSE_UP 2
#define GUI_EVENT_MOUSE_MOVE 3
#define GUI_WIDGET_TEXTBOX 3
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {int unused;} gui_desktop_t;
typedef struct {int unused;} gui_window_t;
typedef struct {int type,x,y,button;} gui_event_t;
typedef void (*gui_widget_callback_t)(gui_window_t*,uint32_t);
typedef struct {int type,style;gui_rect_t bounds;bool selected,visible,enabled,pressed,hovered,range_changed;int range_min,range_max,range_step,range_value,range_start;gui_widget_callback_t callback;} gui_widget_t;
static gui_widget_t made;static int notifications,damage;
static void notify(gui_window_t*w,uint32_t id){(void)w;(void)id;notifications++;}
static gui_desktop_t *api_resolve_desktop(gui_desktop_t*d){return d;}
static gui_widget_t *gui_widget_create_button(gui_desktop_t*d,gui_window_t*w,gui_rect_t b,const char*t,gui_widget_callback_t c){(void)d;(void)w;(void)t;made=(gui_widget_t){.visible=true,.enabled=true,.bounds=b,.callback=c};return &made;}
static void widget_invalidate_owner(gui_widget_t*w){(void)w;damage++;}
static void widget_invalidate_bounds(gui_window_t*w,gui_widget_t*p){(void)w;(void)p;damage++;}
static void gui_widget_set_selected(gui_widget_t*w,bool v){w->selected=v;}
static gui_rect_t gui_widget_screen_bounds(gui_window_t*w,gui_widget_t*p){(void)w;return p->bounds;}
static bool gui_rect_contains(gui_rect_t b,int x,int y){return x>=b.x&&x<b.x+b.w&&y>=b.y&&y<b.y+b.h;}
static void widget_invoke_callback(gui_window_t*w,gui_widget_t*p){p->callback(w,0);}
'''
for n in ['bk_gui_slider_get_value','bk_gui_slider_set_value','bk_gui_choice_set_checked','bk_gui_choice_is_checked','widget_slider_at']:code+=function(widget,n)
for n in ['bk_gui_create_slider','bk_gui_create_choice']:code+=function(api,n)
# Compile the production paths for these new controls together with the
# existing generic button path. Textbox/dropdown paths have separate tests.
handler=function(widget,'gui_widget_handle_event');a=handler.index('    if (widget->type == GUI_WIDGET_TEXTBOX)');b=handler.index('    if (widget->style == GUI_WIDGET_STYLE_LISTBOX)');code+=handler[:a]+handler[b:]
code+=r'''
int main(void){
 gui_window_t window={0};gui_desktop_t desktop={0};gui_rect_t b={100,50,216,24};
 assert(!bk_gui_create_slider(&desktop,&window,b,5,5,1,5,notify));
 assert(!bk_gui_create_slider(&desktop,&window,b,0,10,0,5,notify));
 assert(!bk_gui_create_slider(&desktop,&window,b,0,10,20,5,notify));
 gui_widget_t slider=*bk_gui_create_slider(&desktop,&window,b,0,100,10,50,notify);
 gui_event_t e={GUI_EVENT_MOUSE_DOWN,108,62,2};
 assert(!gui_widget_handle_event(&window,&slider,&e));assert(slider.range_value==50);
 e.button=1;assert(gui_widget_handle_event(&window,&slider,&e));assert(slider.pressed&&slider.range_value==0);
 e.type=GUI_EVENT_MOUSE_MOVE;e.x=900;assert(gui_widget_handle_event(&window,&slider,&e));assert(slider.range_value==100);assert(!notifications);
 e.type=GUI_EVENT_MOUSE_UP;assert(gui_widget_handle_event(&window,&slider,&e));assert(notifications==1&&!slider.pressed);
 assert(!gui_widget_handle_event(&window,&slider,&e));assert(notifications==1);
 bk_gui_slider_set_value(&slider,43);assert(bk_gui_slider_get_value(&slider)==40);
 bk_gui_slider_set_value(&slider,-999);assert(slider.range_value==0);
 bk_gui_slider_set_value(&slider,999);assert(slider.range_value==100);
 slider.enabled=false;e.type=GUI_EVENT_MOUSE_DOWN;e.x=108;assert(!gui_widget_handle_event(&window,&slider,&e));
 gui_widget_t uneven=*bk_gui_create_slider(&desktop,&window,b,0,10,3,10,notify);
 assert(uneven.range_value==10);bk_gui_slider_set_value(&uneven,99);assert(uneven.range_value==10);
 gui_widget_t check=*bk_gui_create_choice(&desktop,&window,b,"Images",false,notify);
 e=(gui_event_t){GUI_EVENT_MOUSE_DOWN,110,62,1};assert(gui_widget_handle_event(&window,&check,&e));
 e.type=GUI_EVENT_MOUSE_UP;assert(gui_widget_handle_event(&window,&check,&e));assert(bk_gui_choice_is_checked(&check));assert(notifications==2);
 e.type=GUI_EVENT_MOUSE_DOWN;gui_widget_handle_event(&window,&check,&e);e.type=GUI_EVENT_MOUSE_UP;e.x=0;gui_widget_handle_event(&window,&check,&e);assert(check.selected&&notifications==2);
 bk_gui_choice_set_checked(&check,false);assert(!check.selected);
 gui_widget_t radio=*bk_gui_create_choice(&desktop,&window,b,"US",true,notify);
 for(int i=0;i<2;i++){e=(gui_event_t){GUI_EVENT_MOUSE_DOWN,110,62,1};gui_widget_handle_event(&window,&radio,&e);e.type=GUI_EVENT_MOUSE_UP;gui_widget_handle_event(&window,&radio,&e);assert(radio.selected);}
 puts("PASS native controls: bounds, quantization, drag outside, release notification, disabled state, checkbox and radio semantics");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-controls-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
