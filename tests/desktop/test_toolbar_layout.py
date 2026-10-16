#!/usr/bin/env python3
"""Check actual window sizing with sequentially created icon/text toolbars."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'gui/windows.c').read_text()
def function(signature):
 start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  if source[end]=='{':depth+=1
  if source[end]=='}':depth-=1
  end+=1
 return source[start:end]+'\n'
code=r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define GUI_BORDER_SIZE 3
#define GUI_TITLEBAR_HEIGHT 20
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct {struct {int width,height;} surface;} gui_desktop_t;
typedef struct gui_widget {gui_rect_t bounds;struct gui_widget *next;} gui_widget_t;
typedef struct {
 bool layout_min_explicit,layout_guard_enabled,borderless,dirty;
 int layout_floor_w,layout_floor_h,layout_authored_w,layout_authored_h,min_w,min_h;
 gui_rect_t bounds;gui_widget_t *widgets;gui_desktop_t *desktop;
} gui_window_t;
static int gui_window_content_top(gui_window_t *w){return w->borderless?0:20;}
'''
code+=function('static void gui_window_fit_layout_to_workarea(')
code+=function('void gui_window_refresh_layout_limits(')
code+=r'''
int main(void){
 gui_desktop_t desktop={{800,600}};
 gui_window_t window={.desktop=&desktop,.bounds={50,50,640,380},.layout_guard_enabled=true,
                     .layout_authored_w=640,.layout_authored_h=380};
 gui_widget_t buttons[5]={0};
 /* Same creation order and dimensions as FILE.BEX. Refresh after creation
    and again after assigning an icon, just like the public widget API. */
 for(int i=0;i<5;++i){
  buttons[i].bounds=(gui_rect_t){6+32*i,28,26,24};buttons[i].next=window.widgets;window.widgets=&buttons[i];
  for(int repeat=0;repeat<2;++repeat)gui_window_refresh_layout_limits(&window);
  for(int j=0;j<=i;++j)assert(buttons[j].bounds.w==26 && buttons[j].bounds.x==6+32*j);
 }
 /* Authored text buttons retain their widths and gaps too. */
 gui_widget_t editor[4]={0};window.widgets=NULL;
 for(int i=0;i<4;++i){editor[i].bounds=(gui_rect_t){6+78*i,60,70,24};editor[i].next=window.widgets;window.widgets=&editor[i];gui_window_refresh_layout_limits(&window);}
 for(int pass=0;pass<8;++pass)gui_window_refresh_layout_limits(&window);
 for(int i=0;i<4;++i)assert(editor[i].bounds.w==70 && editor[i].bounds.x==6+78*i);
 assert(window.min_w==640 && window.min_h==380);
 desktop.surface.width=320;desktop.surface.height=240;gui_window_refresh_layout_limits(&window);
 assert(window.min_w==320 && window.min_h==168 && window.bounds.w==320);
 for(int i=0;i<4;++i)assert(editor[i].bounds.w==70 && editor[i].bounds.x==6+78*i);
 puts("PASS authored toolbar sizes, creation-order independence, text button spacing and small displays");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-toolbar-') as temp:
 path=Path(temp);(path/'test.c').write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
