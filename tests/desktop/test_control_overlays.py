#!/usr/bin/env python3
"""Verify real widget paint/dispatch ordering when a dropdown covers another control."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(source,name):
 pos=source.index(name+'(');start=source.rfind('\n',0,pos)+1;opening=source.index('{',pos);depth=1;end=opening+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
code=r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define GUI_WIDGET_STYLE_DROPDOWN 3
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {int unused;} gui_surface_t;
typedef struct {int unused;} gui_event_t;
typedef struct widget {int id,style;bool expanded;struct widget*next;} gui_widget_t;
typedef struct {bool visible;gui_widget_t *widgets;} gui_window_t;
static int painted[8],paint_count,events[8];
static bool gui_widget_is_dropdown_expanded(gui_widget_t*w){return w->expanded;}
static gui_rect_t gui_window_content_rect(gui_window_t*w){(void)w;return (gui_rect_t){0,0,300,300};}
static bool gui_rect_intersect(gui_rect_t a,gui_rect_t b,gui_rect_t*out){(void)b;*out=a;return true;}
static void gui_widget_paint(gui_surface_t*s,gui_window_t*w,gui_widget_t*p,gui_rect_t c){(void)s;(void)w;(void)c;painted[paint_count++]=p->id;}
static bool gui_widget_handle_event(gui_window_t*w,gui_widget_t*p,const gui_event_t*e){(void)w;(void)e;events[p->id]++;return true;}
'''
code+=function((ROOT/'gui/windows.c').read_text(),'gui_window_paint_widgets')
code+=function((ROOT/'system/desktop/deskmanager.c').read_text(),'deskmanager_dispatch_widgets')
code+=r'''
int main(void){gui_widget_t lower={.id=3,.style=3},button={.id=2,.next=&lower},top={.id=1,.style=3,.expanded=true,.next=&button};gui_window_t w={.visible=true,.widgets=&top};gui_surface_t s={0};gui_event_t e={0};
 gui_window_paint_widgets(&s,&w,(gui_rect_t){0,0,300,300});assert(paint_count==3&&painted[2]==1);assert(painted[0]==2&&painted[1]==3);
 assert(deskmanager_dispatch_widgets(&w,&e));assert(events[1]==1&&events[2]==0&&events[3]==0);
 top.expanded=false;deskmanager_dispatch_widgets(&w,&e);assert(events[2]==1&&events[3]==1);
 puts("PASS dropdown overlays: expanded list paints last and receives exclusive pointer events");}
'''
with tempfile.TemporaryDirectory(prefix='bk-overlays-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
