#!/usr/bin/env python3
"""Check production process priority updates and thread publication inheritance."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/sched/task.c').read_text()
priority=s[s.index('uint32_t task_process_priority('):]
quantum=s[s.index('static uint32_t task_quantum_limit('):s.index('static void task_watchdog_tick(')]
inherit=s[s.index('    /* A cooperative process stop may arrive'):s.index('    yield_tick[index] = 0;',s.index('    /* A cooperative process stop may arrive'))]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define TASK_MAX 4
#define TASK_UNUSED 0
#define TASK_READY 1
#define TASK_ZOMBIE 4
#define TASK_QUANTUM_MILLISECONDS 20
static int g_task_table_lock;
static uint32_t g_quantum_limit_ticks;
typedef struct {uint32_t pid,process_id,priority,state;bool system,exit_requested;int32_t exit_code;} task_t;
static task_t tasks[4],*creator;
static task_t *task_current_entry(void){return creator;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;}
static uint32_t pit_get_frequency_hz(void){return 100;}
'''+priority+quantum+'static void inherit(task_t *task,uint32_t process_id,bool is_thread,bool user){\n'+inherit+'}\n'+r'''
int main(void){
 tasks[0]=(task_t){.pid=10,.process_id=10,.state=TASK_READY};
 tasks[1]=(task_t){.pid=11,.process_id=10,.state=TASK_READY};
 assert(task_process_priority(10,0)==2&&task_process_priority(10,3)==3);
 assert(tasks[0].priority==3&&tasks[1].priority==3);
 creator=&tasks[0];assert(task_quantum_limit()==4);
 assert(task_process_priority(10,1)==1&&task_quantum_limit()==1);
 tasks[1].system=true;assert(task_process_priority(10,3)==0);
 assert(tasks[0].priority==1&&tasks[1].priority==1);
 assert(!task_process_priority(0,1)&&!task_process_priority(10,4));
 tasks[0].priority=3;tasks[0].exit_requested=true;tasks[0].exit_code=7;
 task_t child={0};inherit(&child,10,true,true);
 assert(child.priority==3&&child.exit_requested&&child.exit_code==7);
 child=(task_t){0};inherit(&child,20,true,true);assert(!child.exit_requested&&!child.priority);
 child=(task_t){0};inherit(&child,10,false,true);assert(!child.exit_requested&&!child.priority);
 puts("PASS process-wide priority, atomic system protection, bounded levels, quantum weighting and stop/priority inheritance for new threads");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-priority-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
