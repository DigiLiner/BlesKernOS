#!/usr/bin/env python3
"""Production shared busy cursor and descriptor-owned launch references."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'gui/compositor.c').read_text();s=s[s.index('static bool g_cursor_busy_was_active;'):s.index('bool gui_compositor_tick_cursor')].replace('static bool g_cursor_busy_was_active;','')
g=(root/'kernel/linux/i386_ipc.inc').read_text();g=g[g.index('static void bles_gfx_launch_busy('):g.index('static int32_t bles_gfx_ioctl(')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef enum {GUI_CURSOR_ARROW,GUI_CURSOR_PROGRESS,GUI_CURSOR_TEXT} gui_cursor_style_t;
typedef struct {uint32_t cursor_busy_count;bool cursor_custom;gui_cursor_style_t cursor_style;} gui_desktop_t;
static gui_desktop_t state,*desktop=&state;
static gui_desktop_t *gui_get_desktop(void){return desktop;}
static void gui_request_paint(void){}
typedef struct {bool launch_busy;int launch_lock;} bles_gfx_context;
static void kmutex_lock(int *p){assert(!*p);*p=1;}
static void kmutex_unlock(int *p){assert(*p);*p=0;}
''' + s + g + r'''
int main(void){
 bles_gfx_context a={0},b={0};
 state.cursor_custom=true;state.cursor_style=GUI_CURSOR_TEXT;
 assert(compositor_custom_cursor(&state));
 bles_gfx_launch_busy(&a,true);bles_gfx_launch_busy(&a,true);
 assert(state.cursor_busy_count==1 && compositor_cursor_style(&state)==GUI_CURSOR_PROGRESS);
 assert(!compositor_custom_cursor(&state));
 bles_gfx_launch_busy(&b,true);assert(state.cursor_busy_count==2);
 bles_gfx_launch_busy(&a,false);assert(state.cursor_busy_count==1);
 bles_gfx_launch_busy(&a,false);assert(state.cursor_busy_count==1);
 bles_gfx_launch_busy(&b,false);assert(!state.cursor_busy_count);
 assert(compositor_custom_cursor(&state) && compositor_cursor_style(&state)==GUI_CURSOR_TEXT);
 gui_compositor_busy_end();assert(!state.cursor_busy_count);
 gui_compositor_busy_begin();gui_compositor_busy_begin();gui_compositor_busy_end();assert(state.cursor_busy_count==1);
 gui_compositor_busy_end();assert(!state.cursor_busy_count);
 desktop=NULL;gui_compositor_busy_begin();gui_compositor_busy_end();assert(!compositor_busy(NULL));
 puts("PASS shared launch cursor: concurrent references, duplicate begin/end, custom cursor override/restore, loader nesting and underflow");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-launch-cursor-') as temp:
 p=Path(temp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
