#!/usr/bin/env python3
"""Exercise the production Vision loop with delayed GPU completion and frame cadence."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'system/services/vision_engine.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include '))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int w,h; } gui_rect_t;
typedef struct { int unused; } gfx_info_t;
typedef struct { int unused; } gui_desktop_t;
typedef struct { int unused; } gui_event_queue_t;
typedef struct { int type,dy; } gui_event_t;
typedef struct {uint32_t ticks,frequency,paint_generation;bool layout_refresh;} bk_vision_frame_t;
#define GUI_EVENT_MOUSE_MOVE 1
#define GUI_EVENT_MOUSE_WHEEL 2
#define SYSTEM_SOUND_SCROLL 0
#define PERF_SCOPE_GUI_EVENTS 0
#define PERF_SCOPE_GUI_FRAME 1
#define kprintf(...) ((void)0)
static uint32_t now,wait_ticks,paints,dispatches;
static bool pending,complete,dirty;
static bool bk_gfx_info(gfx_info_t *p){(void)p;return true;}
static bool bk_proc_exit_requested(void){return false;}
static uint32_t bk_sys_ticks(void){return now;}
static void bk_sys_sleep_ms(unsigned n){(void)n;}
static bool bk_vision_frame_begin(bk_vision_frame_t *f){*f=(bk_vision_frame_t){now,300,0,false};return true;}
static void vision_windows_dispatch(gui_desktop_t *d){(void)d;}
static void vision_native_frames_dispatch(gui_desktop_t *d){(void)d;dispatches++;if(pending && complete){dirty=true;pending=false;}}
static gui_rect_t deskbar_clock_rect(const gui_desktop_t *d){(void)d;return (gui_rect_t){0,0};}
static void gui_event_init(gui_event_queue_t *e){(void)e;}
static void deskmanager_refresh_layout(void){}
static void perfmon_gui_loop(void){}
static uint64_t perfmon_scope_begin(void){return 0;}
static void gui_event_poll(gui_event_queue_t *e){(void)e;}
static bool gui_event_next(gui_event_queue_t *e,gui_event_t *v){(void)e;(void)v;return false;}
static void perfmon_gui_event(bool b){(void)b;}
static void bk_vision_note_input(void){}
static int system_sound_event_play(unsigned n){(void)n;return 0;}
static void gui_desktop_handle_event(gui_desktop_t *d,gui_event_t *e){(void)d;(void)e;}
static bool gui_compositor_move_hardware_cursor(gui_desktop_t *d){(void)d;return true;}
static bool gui_compositor_tick_cursor(gui_desktop_t *d,uint32_t t){(void)d;(void)t;return false;}
static bool gui_desktop_tick_minimize_animation(gui_desktop_t *d,uint32_t t){(void)d;(void)t;return false;}
static bool gui_desktop_has_dirty(gui_desktop_t *d){(void)d;return dirty;}
static void gui_desktop_invalidate_all(gui_desktop_t *d){(void)d;dirty=true;}
static void gui_desktop_invalidate_rect(gui_desktop_t *d,gui_rect_t r){(void)d;(void)r;dirty=true;}
static void perfmon_scope_end(unsigned n,uint64_t t){(void)n;(void)t;}
static uint32_t compat_mode_gui_target_fps(void){return 60;}
static uint32_t compat_mode_gui_idle_hz(void){return 50;}
static bool bk_vision_native_pending(void){return pending;}
static void gui_desktop_paint(gui_desktop_t *d){(void)d;paints++;dirty=false;}
static void bk_vision_frame_end(void){}
static void bk_vision_ipc_wait(unsigned ticks){wait_ticks=ticks;}
''' + source + r'''
int main(void){
 gui_desktop_t d={0};
 assert(vision_engine_step(&d) && wait_ticks==6 && paints==0);
 pending=true;now=5;
 assert(vision_engine_step(&d) && wait_ticks==6 && paints==0 && dispatches==1);
 /* An incomplete capture must not spend a presentation period. */
 now=6;complete=true;
 assert(vision_engine_step(&d) && paints==1 && dispatches==2 && wait_ticks==6);
 /* A new frame waits for the next scheduled period; no burst of copies. */
 pending=true;now=7;
 assert(vision_engine_step(&d) && paints==1 && dispatches==2 && wait_ticks==6);
 now=10;assert(vision_engine_step(&d) && paints==2 && dispatches==3 && wait_ticks==6);
 /* A missing panel clock must not invalidate the whole desktop every second. */
 now=300;assert(vision_engine_step(&d) && paints==2 && wait_ticks==6);
 dirty=true;now=301;assert(vision_engine_step(&d) && paints==3);
 puts("PASS: incomplete captures preserve cadence, completed frames paint once, idle desktop retains timeout and no phantom clock damage");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vision-cadence-') as directory:
 path=Path(directory);(path/'test.c').write_text(code)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
