#!/usr/bin/env python3
"""Production IRQ queue and GUI decoding preserve a click between frames."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
core=(root/'kernel/drivers/input/mouse_core.c').read_text();core=core[core.index('#define MOUSE_EVENTS'):core.index('static int32_t mouse_clamp')]
gui=(root/'gui/event.c').read_text();gui=gui[gui.index('static void poll_mouse_snapshot'):gui.index('void gui_event_poll')]
code='''#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#undef NULL
#include "kernel/include/mouse.h"
#include "gui/gui.h"
typedef pthread_mutex_t kspinlock_t;
#define KSPINLOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define BK_INPUT_SWAP_BUTTONS 0
static bool swapped,g_injected_active;
static bool bk_input_preference_get(int key){(void)key;return swapped;}
static uint32_t kspin_lock_irqsave(kspinlock_t *lock){assert(!pthread_mutex_lock(lock));return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *lock,uint32_t flags){(void)flags;assert(!pthread_mutex_unlock(lock));}
static gui_event_t decoded[32];static uint32_t decoded_count;
static void push_event(gui_event_queue_t *queue,gui_event_t event){(void)queue;assert(decoded_count<32);decoded[decoded_count++]=event;}
'''+core+gui+'''
int main(void){mouse_state_t m={0},record;gui_event_queue_t q={0};m.x=10;m.y=20;
 mouse_publish_state(&m);m.buttons=1;mouse_publish_state(&m);
 m.x=15;mouse_publish_state(&m);m.buttons=0;mouse_publish_state(&m);
 while(mouse_pop_event(&record))poll_mouse_snapshot(&q,record);
 bool down=false,up=false;
 for(uint32_t n=0;n<decoded_count;n++){
  if(decoded[n].type==GUI_EVENT_MOUSE_DOWN){assert(decoded[n].x==10 && !down);down=true;}
  if(decoded[n].type==GUI_EVENT_MOUSE_UP){assert(down && decoded[n].x==15);up=true;}
 }
 assert(down && up && q.last_mouse_buttons==0);
 for(int n=0;n<500;n++){m.x=n;mouse_publish_state(&m);}
 assert(mouse_pop_event(&record) && record.x==499 && !mouse_pop_event(&record));
 for(int n=0;n<200;n++){m.buttons=n&1;mouse_publish_state(&m);}
 uint32_t count=0;while(mouse_pop_event(&record))count++;assert(count==63 && record.buttons==1);
 swapped=true;m.buttons=0;mouse_publish_state(&m);m.buttons=1;mouse_publish_state(&m);
 assert(mouse_pop_event(&record) && record.buttons==0);assert(mouse_pop_event(&record) && record.buttons==2);
 puts("PASS complete click between frames, press coordinate, movement coalescing, bounded overflow and swapped buttons");}
'''
with tempfile.TemporaryDirectory(prefix='bk-mouse-') as d:
 d=Path(d);source=d/'test.c';source.write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-I'+str(root),str(source),'-pthread','-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
