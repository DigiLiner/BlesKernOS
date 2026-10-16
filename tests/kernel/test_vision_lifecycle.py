#!/usr/bin/env python3
"""Exercise production Vision ownership cleanup, including abnormal exits."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'gui/gui.c').read_text()
claim=src[src.index('bool gui_vision_claim_service('):src.index('\nbool gui_vision_initialize_engine(')]
cleanup=src[src.index('void gui_vision_process_cleanup('):src.index('\nvoid gui_init(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
static volatile uint32_t g_vision_owner_pid;
static volatile bool g_vision_ready;
static struct {bool initialized;} g_vision_loop;
static bool g_gui_shell_ready,g_vision_layout_refresh_pending;
static uint32_t pid;
static void vision_ipc_server_offline(uint32_t owner) {(void)owner;}
static uint32_t task_current_process_id(void) {return pid;}
#define kprintf(...) ((void)0)
'''+claim+cleanup+r'''
int main(void) {
 pid=0;assert(!gui_vision_claim_service());
 pid=6;assert(gui_vision_claim_service());g_vision_ready=true;g_vision_loop.initialized=true;g_gui_shell_ready=true;g_vision_layout_refresh_pending=true;
 gui_vision_process_cleanup(0);gui_vision_process_cleanup(12);
 assert(g_vision_owner_pid==6 && g_vision_ready && g_vision_loop.initialized);
 pid=12;assert(!gui_vision_claim_service());
 /* Reaper runs as another process: do not depend on current PID. */
 gui_vision_process_cleanup(6);
 assert(!g_vision_owner_pid && !g_vision_ready && !g_vision_loop.initialized);
 assert(!g_gui_shell_ready && !g_vision_layout_refresh_pending);
 assert(gui_vision_claim_service() && g_vision_owner_pid==12);
 gui_vision_release_service();assert(!g_vision_owner_pid);
 gui_vision_process_cleanup(6);assert(!g_vision_owner_pid);
 puts("PASS abnormal owner cleanup, unrelated process isolation, reclaim and graceful release");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vision-') as d:
    d=Path(d);c=d/'test.c';c.write_text(code);binary=d/'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
# The worker must be defined inside Vision, rather than imported from CPL0.
subprocess.run(['nm','--defined-only',str(root/'build/system/services/vision.o')],check=True,stdout=subprocess.PIPE)
symbols=subprocess.check_output(['nm','--defined-only',str(root/'build/system/services/vision.o')],text=True)
assert ' t screensaverd_main' in symbols and ' T screensaverd_install' in symbols
print('PASS screensaver worker and installer linked into Vision executable')
