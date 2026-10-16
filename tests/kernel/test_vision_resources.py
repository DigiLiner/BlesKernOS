#!/usr/bin/env python3
"""Independent kernel lifetime records survive corrupted graphical links."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/api/vision_resources.c').read_text();source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code='''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#undef NULL
#include "gui/gui.h"
typedef pthread_mutex_t kspinlock_t;
#define KSPINLOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static uint32_t kspin_lock_irqsave(kspinlock_t *lock){assert(!pthread_mutex_lock(lock));return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *lock,uint32_t flags){(void)flags;assert(!pthread_mutex_unlock(lock));}
static uint32_t task_current_process_id(void){return 5;}
static size_t mm_allocation_size(const void *object){return object?65536:0;}
void gui_desktop_invalidate_all(gui_desktop_t *d){(void)d;}
void gui_request_paint(void){}
'''+source+'''
int main(void){gui_desktop_t d={0};gui_window_t a={0},b={0};gui_program_t p={0};
 assert(bk_vision_resource_register(&a,false));assert(bk_vision_resource_register(&b,false));assert(bk_vision_resource_register(&p,true));
 bk_vision_resource_owner(&a,7);bk_vision_resource_owner(&b,8);
 d.first_window=&a;d.last_window=&b;a.next=&b;b.prev=&a;d.first_program=d.last_program=&p;
 assert(vision_resources_checkpoint(&d));
 d.first_window=(void *)0xdeadbeef;d.last_program=(void *)0xcafebabe;a.next=(void *)0xdeadbeef;b.prev=(void *)0xcafebabe;
 assert(!vision_resources_checkpoint(&d));vision_resources_recover(&d);
 assert(d.first_window==&a && a.next==&b && b.prev==&a && !b.next && d.last_window==&b);
 assert(a.owner_pid==7 && b.owner_pid==8 && d.first_program==&p && d.last_program==&p);
 bk_vision_resource_retire(&a);bk_vision_resource_forget(&p);vision_resources_recover(&d);
 assert(d.first_window==&b && d.last_window==&b && !b.prev && !d.first_program && !d.last_program);
 assert(vision_resources_checkpoint(&d));puts("PASS invalid-link rejection, independent reconstruction, preserved owners and retired-object exclusion");}
'''
with tempfile.TemporaryDirectory(prefix='bk-resources-') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror','-I'+str(root),str(d/'test.c'),'-pthread','-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
