#!/usr/bin/env python3
"""Exercise production cancellation when an app dies without returning from a GUI callback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/sched/task.c').read_text()
f=s[s.index('static void task_release_upcall_buffers('):s.index('#include "../linux/i386_signal.inc"')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define TASK_UPCALL_QUEUE 8
#define kmemset memset
typedef struct {uint32_t destroy_state;bool content_pending,content_repaint;int ends,destroys;} gui_window_t;
typedef struct {void *window_target,*release_after_upcall;int payload_argument;} task_upcall_t;
typedef struct {bool upcall_active;uint8_t upcall_tail,upcall_head;task_upcall_t active_upcall,upcalls[TASK_UPCALL_QUEUE];} task_t;
static int g_task_table_lock,desktop_depth,freed;
static task_t *current;
static void gui_desktop_paint_lock(void){desktop_depth++;}
static void gui_desktop_paint_unlock(void){desktop_depth--;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
static void kfree(void *p){assert(desktop_depth==1);free(p);freed++;}
static void gui_window_end_content_paint(gui_window_t *w){assert(desktop_depth==1&&!current->upcall_active);w->ends++;}
static void gui_window_destroy(gui_window_t *w){assert(desktop_depth==1&&!current->upcall_active&&current->upcall_head==current->upcall_tail);assert(!w->destroys);w->destroys++;w->destroy_state=2;}
'''+f+r'''
int main(void){
 gui_window_t retired={.destroy_state=1,.content_pending=true,.content_repaint=true},live={.content_pending=true};
 task_t t={.upcall_active=true,.upcall_tail=6,.upcall_head=1};current=&t;
 t.active_upcall=(task_upcall_t){&retired,malloc(1),-2};
 t.upcalls[6]=(task_upcall_t){&retired,malloc(1),0};
 t.upcalls[7]=(task_upcall_t){&live,malloc(1),-2};
 t.upcalls[0]=(task_upcall_t){NULL,malloc(1),0};
 task_release_upcall_buffers(&t);
 assert(!t.upcall_active&&t.upcall_head==0&&t.upcall_tail==0);
 assert(retired.ends==1&&retired.destroys==1&&!retired.content_pending&&!retired.content_repaint);
 assert(live.ends==1&&!live.destroys&&!live.content_pending&&freed==4&&!desktop_depth);
 task_release_upcall_buffers(&t);assert(retired.destroys==1&&freed==4);
 t.upcall_head=7;for(int i=0;i<7;i++)t.upcalls[i]=(task_upcall_t){&live,malloc(1),-2};
 task_release_upcall_buffers(&t);assert(live.ends==2&&freed==11);
 puts("PASS active and queued paints release staging; unlinked tombstones finalize once; payloads release once");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-upcall-cleanup-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
