#!/usr/bin/env python3
"""A first native GPU frame must initialize the scene without a timer/input."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'system/services/vision_windows.c').read_text()
dispatch = source[source.index('void vision_native_frames_dispatch('):source.index('void vision_windows_dispatch(')]
publish = source[source.index('static bool publish_gpu_frame('):source.index('void vision_native_frames_dispatch(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} gui_rect_t;
typedef struct window {
    struct window *next;
    bool gpu_view_visible,gpu_view_frame_dirty,dirty;
    uint32_t gpu_view_surface,owner_pid;
    uint16_t gpu_view_width,gpu_view_height;
    gui_rect_t gpu_view_rect;
} gui_window_t;
typedef struct {gui_window_t *first_window;} gui_desktop_t;
typedef struct {uint32_t values[3];gui_rect_t bounds;} bk_vision_request_t;
static bool pending=true;
static uint32_t surface=42,invalidations,requests,copies;
static gui_rect_t bounds={12,34,640,480};
static bool bk_vision_native_pending(void){return pending;}
static uint32_t gui_gpu_compositor_native_viewport(gui_window_t *w,uint16_t *width,uint16_t *height){
    (void)w;copies++;*width=640;*height=480;return surface;
}
static gui_rect_t gui_window_content_rect(gui_window_t *w){(void)w;return bounds;}
static void gui_desktop_invalidate_all(gui_desktop_t *d){assert(d);invalidations++;}
static void gui_request_paint(void){requests++;}
static bool gui_gpu_compositor_publish_viewport(gui_window_t *w,uint32_t s,uint16_t width,uint16_t height){
    (void)w;(void)s;(void)width;(void)height;return true;
}
#define kprintf(...) ((void)0)
''' + publish + dispatch + r'''
int main(void){
    gui_window_t w={0};gui_desktop_t d={&w};
    vision_native_frames_dispatch(&d);
    assert(w.gpu_view_visible && w.gpu_view_frame_dirty && w.dirty);
    assert(w.gpu_view_surface==42 && invalidations==1 && requests==1);
    /* No clock exists in this panel: first attachment itself must request
     * the shell background capture. Subsequent frames reuse that scene. */
    w.dirty=false;w.gpu_view_frame_dirty=false;
    vision_native_frames_dispatch(&d);
    assert(w.gpu_view_frame_dirty && !w.dirty && invalidations==1 && requests==1);
    bounds.x++;vision_native_frames_dispatch(&d);
    assert(invalidations==2 && requests==2 && w.dirty);
    pending=false;vision_native_frames_dispatch(&d);assert(copies==3);
    pending=true;surface=0;w.dirty=false;w.gpu_view_frame_dirty=false;
    vision_native_frames_dispatch(&d);
    assert(!w.dirty && !w.gpu_view_frame_dirty && invalidations==2);
    gui_window_t legacy={0};bk_vision_request_t r={{44,320,200},{0,0,320,200}};
    assert(publish_gpu_frame(&d,&legacy,&r));
    assert(legacy.gpu_view_visible && legacy.gpu_view_frame_dirty && invalidations==3);
    legacy.dirty=false;assert(publish_gpu_frame(&d,&legacy,&r));
    assert(!legacy.dirty && invalidations==3);
    r.values[0]=0;assert(!publish_gpu_frame(&d,&legacy,&r) && invalidations==3);
    puts("PASS: first GPU attachment initializes scene without clock/input; ordinary frames reuse it; geometry changes and unavailable captures handled");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-native-scene-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
