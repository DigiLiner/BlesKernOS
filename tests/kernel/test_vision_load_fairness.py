#!/usr/bin/env python3
"""Check the compositor wait indication used at completed Linux syscall boundaries."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'gui/gui.c').read_text()
part=s[s.index('static volatile uint32_t g_vision_frame_tid'):s.index('void bk_vision_frame_end(')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define KDOMAIN_TASK 1U
#define KDOMAIN_VFS 2U
#define KDOMAIN_GUI 4U
#define KDOMAIN_GFX 8U
typedef struct {bool layout_refresh;uint32_t ticks,frequency,paint_generation,last_input;} bk_vision_frame_t;
static unsigned g_vision_owner_pid=7,tid=7,g_mode_layout_refresh_pending,g_paint_generation=9,g_last_input_tick=10;
static bool g_vision_ready=true,splash,entered,paint_locked,blocked;
static void *g_desktop;
static uint32_t task_current_pid(void){return tid;}
static uint32_t task_current_process_id(void){return 7;}
static bool bootsplash_active(void){return splash;}
static void vision_file_dialog_poll(void){}
bool gui_vision_frame_waiting(void);
static void kernel_domains_enter(unsigned mask){assert(mask==15 && gui_vision_frame_waiting());entered=true;}
static void kernel_domains_exit(unsigned mask){(void)mask;}
static void gui_desktop_paint_lock(void){assert(entered && gui_vision_frame_waiting());paint_locked=true;}
static void task_block_user_upcalls(bool value){blocked=value;}
static void gui_desktop_invalidate_all(void *p){(void)p;}
static void gui_request_paint(void){}
static unsigned pit_get_ticks(void){return 100;}
static unsigned pit_get_frequency_hz(void){return 300;}
'''+part+r'''
int main(void){
 bk_vision_frame_t f={0};assert(!gui_vision_frame_waiting());
 assert(!bk_vision_frame_begin(NULL) && !entered);
 tid=8;assert(!bk_vision_frame_begin(&f) && !entered);tid=7;
 splash=true;assert(!bk_vision_frame_begin(&f));splash=false;
 assert(bk_vision_frame_begin(&f) && entered && paint_locked && blocked && !gui_vision_frame_waiting());
 assert(f.ticks==100 && f.frequency==300 && f.paint_generation==9 && f.last_input==10);
 assert(!bk_vision_frame_begin(&f));
 g_vision_waiting_tid=7;assert(gui_vision_frame_waiting());
 g_vision_ready=false;assert(!gui_vision_frame_waiting());g_vision_ready=true;
 g_vision_owner_pid=8;assert(!gui_vision_frame_waiting());
 puts("PASS actual frame entry announces a waiting desktop only across lock acquisition; no wait on rejected/nested entry, offline service or replacement owner");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-frame-wait-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
