#!/usr/bin/env python3
"""Use production handoff/wakeup code to prevent early wake and stack reaping."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/sched/task.c').read_text()
wake=s[s.index('static void task_wake_sleepers_smp'):s.index('static uint32_t task_quantum_limit')]
start=s.index('        if (old_slot >= 0 && !current->idle)')
handoff=s[start:s.index('        if (next < 0)',start)]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define TASK_MAX 4
#define SMP_MAX_CPUS 2
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_SLEEPING 3
#define TASK_ZOMBIE 5
typedef struct {uint32_t futex_key;} linux_signals;
typedef struct {int state,running_cpu,queued_cpu;uint32_t wake_tick;bool idle;void *context;linux_signals *linux_signals;} task_t;
typedef struct {int deferred_ready_slot;} task_cpu_state_t;
static task_t tasks[TASK_MAX];
static task_cpu_state_t cpu_states[2];
static int g_task_table_lock, enqueued;
static bool locked, stack_left;
static uint32_t now=100;
static uint32_t pit_get_ticks(void){return now;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;assert(!locked);locked=true;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;assert(locked);locked=false;}
static bool scheduler_smp_enqueue(int slot,int cpu,bool wake){(void)cpu;(void)wake;assert(stack_left && !locked && tasks[slot].state==TASK_READY && tasks[slot].running_cpu==-1);enqueued++;return true;}
static void task_fpu_restore(task_t *t){(void)t;}
#define TASK_SCHEDULE_FINISH(x) do{(void)(x);return;}while(0)
''' + wake + r'''
static void prepare(task_t *current,unsigned cpu,int old_slot,int next){
''' + handoff + r'''
}
static bool can_reap(task_t *t){return t->state==TASK_ZOMBIE && t->running_cpu==-1;}
int main(void){
 for(int state=TASK_RUNNING;state<=TASK_ZOMBIE;state++){
  if(state==4)continue;
  cpu_states[0].deferred_ready_slot=cpu_states[1].deferred_ready_slot=-1;
  tasks[1]=(task_t){.state=state,.running_cpu=1,.queued_cpu=-1,.wake_tick=10};
  enqueued=0;stack_left=false;
  prepare(&tasks[1],1,1,2);
  assert(tasks[1].running_cpu==TASK_CPU_HANDOFF && cpu_states[1].deferred_ready_slot==1);
  task_wake_sleepers_smp(100);assert(!enqueued && !can_reap(&tasks[1]));
  if(state==TASK_SLEEPING)assert(tasks[1].state==TASK_SLEEPING);
  stack_left=true;task_publish_deferred_ready(1);
  assert(tasks[1].running_cpu==-1 && cpu_states[1].deferred_ready_slot==-1);
  if(state==TASK_ZOMBIE)assert(can_reap(&tasks[1]) && !enqueued);
  if(state==TASK_RUNNING)assert(tasks[1].state==TASK_READY && enqueued==1);
  if(state==TASK_SLEEPING){assert(tasks[1].state==TASK_READY && enqueued==1);task_wake_sleepers_smp(100);assert(enqueued==1);}
 }
 /* Future sleeps stay asleep; a producer/deadline during handoff becomes
  * runnable only on the new stack. Include tick wrap and futex expiry. */
 for(unsigned scenario=0;scenario<4;scenario++){
  linux_signals signal={.futex_key=123};
  now=scenario==2?0xfffffff0U:100U;
  uint32_t deadline=scenario==2?5U:scenario==3?0xfffffff0U:101U;
  tasks[1]=(task_t){.state=TASK_SLEEPING,.running_cpu=1,.queued_cpu=-1,.wake_tick=deadline,.linux_signals=&signal};
  cpu_states[1].deferred_ready_slot=-1;enqueued=0;stack_left=false;
  prepare(&tasks[1],1,1,2);
  if(scenario==1)tasks[1].wake_tick=now; /* Event/futex fired during handoff. */
  task_wake_sleepers_smp(now);assert(!enqueued);
  stack_left=true;task_publish_deferred_ready(1);
  bool due=scenario==1 || scenario==3;
  assert(enqueued==(int)due && tasks[1].state==(due?TASK_READY:TASK_SLEEPING));
  assert(signal.futex_key==(due?0U:123U));
  task_publish_deferred_ready(1);assert(enqueued==(int)due);
 }
 /* Exit arriving during the deferred READY interval must not enqueue it. */
 tasks[1]=(task_t){.state=TASK_READY,.running_cpu=TASK_CPU_HANDOFF};
 cpu_states[1].deferred_ready_slot=1;enqueued=0;tasks[1].state=TASK_ZOMBIE;
 task_publish_deferred_ready(1);assert(!enqueued && can_reap(&tasks[1]));
 puts("PASS actual SMP handoff: stacks stay owned until retirement; completed wakes publish immediately; future/wrapped deadlines and futex expiry preserved; no duplicate wake or queued zombie");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-smp-handoff-') as folder:
    path=Path(folder)
    (path/'test.c').write_text(code)
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
