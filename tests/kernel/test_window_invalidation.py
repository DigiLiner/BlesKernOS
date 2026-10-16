#!/usr/bin/env python3
"""An async client invalidated after composition must retain its repaint."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
a=(root/'kernel/api/api.c').read_text();a=a[a.index('void bk_gui_window_invalidate('):a.index('bool bk_gui_window_set_gpu_viewport(')]
c=(root/'gui/compositor.c').read_text();c=c[c.index('static void compositor_finish_windows('):c.index('static void paint_cursor_with_palette(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct window {bool dirty,content_repaint,streaming_content,visible,paint_bounds_valid;unsigned content_pid;void *content_paint;gui_rect_t bounds,paint_bounds;struct window *next;} gui_window_t;
typedef struct {gui_window_t *first_window;} gui_desktop_t;
static unsigned requested;
static void gui_request_paint(void){requested++;}
'''+a+c+r'''
int main(void){
 gui_window_t w={.content_pid=6,.content_paint=(void *)1,.visible=true};
 gui_desktop_t d={&w};
 /* Vision already sampled the previous content, then the client changes it. */
 bk_gui_window_invalidate(&w);compositor_finish_windows(&d);
 assert(w.dirty && w.content_repaint && requested==1);
 /* Queuing a fresh content callback consumes the request. */
 w.content_repaint=false;compositor_finish_windows(&d);assert(!w.dirty);
 /* A second invalidation during that callback must survive its old frame. */
 bk_gui_window_invalidate(&w);compositor_finish_windows(&d);
 assert(w.dirty && w.content_repaint);
 /* Streaming and synchronous clients do not use async content callbacks. */
 w.content_repaint=false;w.streaming_content=true;
 bk_gui_window_invalidate(&w);compositor_finish_windows(&d);assert(!w.dirty && !w.content_repaint);
 w.streaming_content=false;w.content_pid=0;
 bk_gui_window_invalidate(&w);compositor_finish_windows(&d);assert(!w.dirty);
 bk_gui_window_invalidate(NULL);assert(requested==4);
 puts("PASS invalidation survives completion of an older async frame; streaming/synchronous windows keep their completion semantics");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-window-invalidation-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
