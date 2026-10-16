#!/usr/bin/env python3
"""Exercise production FUTEX_WAKE queue publication and stack ownership."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_futex.inc').read_text()
wake=s[s.index('static void linux_wake_locked'):s.index('int32_t task_linux_futex_wait')]
wake+=s[s.index('int32_t task_linux_futex_wake'):]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TASK_MAX 8
#define TASK_SLEEPING 3
#define TASK_READY 1
#define TASK_RUNNING 2
#define BK_EFAULT 14
typedef struct {uint32_t eax;} registers_t;
typedef struct {uint32_t futex_key,futex_process,futex_mask;} linux_signals;
typedef struct {int state,running_cpu,queued_cpu;uint32_t wake_tick;registers_t *context;linux_signals *linux_signals;} task_t;
static task_t tasks[TASK_MAX];static registers_t frames[TASK_MAX];static linux_signals signals[TASK_MAX];
static int g_task_table_lock,queued,locked;static uint32_t rescheduled;static bool smp=true,key_ok=true;
static uint32_t pit_get_ticks(void){return 42;}
static bool syscall_linux_futex_key(uint32_t address,bool priv,uint32_t *key,uint32_t *scope){(void)priv;*key=address;*scope=7;return key_ok;}
static uint32_t kspin_lock_irqsave(int *p){(void)p;assert(!locked);locked=1;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;assert(locked);locked=0;}
static bool smp_scheduler_started(void){return smp;}
static bool scheduler_smp_enqueue(int slot,int cpu,bool notify){assert(!locked && cpu==-1 && notify);assert(tasks[slot].running_cpu==-1 && tasks[slot].state==TASK_READY && tasks[slot].queued_cpu==-1);queued|=1<<slot;return true;}
static void smp_reschedule_mask(uint32_t mask){assert(!locked);rescheduled|=mask;}
''' + wake + r'''
static void reset(void){
 memset(tasks,0,sizeof tasks);memset(frames,0xff,sizeof frames);queued=0;rescheduled=0;smp=true;key_ok=true;
 for(int i=1;i<TASK_MAX;i++){
  signals[i]=(linux_signals){100,7,1};
  tasks[i]=(task_t){TASK_SLEEPING,-1,-1,999,&frames[i],&signals[i]};
 }
}
int main(void){
 reset();tasks[2].running_cpu=1;tasks[3].running_cpu=-2; // live/handoff stacks
 tasks[4].queued_cpu=0;signals[5].futex_mask=2;signals[6].futex_process=8;signals[7].futex_key=101;
 assert(task_linux_futex_wake(100,8,1,false)==4);
 assert(queued==(1<<1) && rescheduled==2);
 assert(tasks[1].state==TASK_READY && tasks[4].state==TASK_READY);
 assert(tasks[2].state==TASK_SLEEPING && tasks[3].state==TASK_SLEEPING);
 for(int i=1;i<=4;i++)assert(!signals[i].futex_key && frames[i].eax==0 && tasks[i].wake_tick==42);
 for(int i=5;i<TASK_MAX;i++)assert(signals[i].futex_key && tasks[i].wake_tick==999);
 assert(task_linux_futex_wake(100,8,1,false)==0); // no double wake
 reset();assert(task_linux_futex_wake(100,0,1,false)==0 && !queued);
 assert(task_linux_futex_wake(100,2,1,false)==2 && queued==6);
 reset();smp=false;assert(task_linux_futex_wake(100,1,1,false)==1 && !queued && tasks[1].state==TASK_READY);
 reset();key_ok=false;assert(task_linux_futex_wake(100,1,1,false)==-BK_EFAULT && !queued);
 reset();tasks[1].state=TASK_RUNNING;assert(task_linux_futex_wake(100,1,1,false)==1 && queued==(1<<2));
 puts("PASS FUTEX_WAKE: retired stacks ready immediately; live/handoff stacks retained; masks, scope, limits, UP and repeat wake verified");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-futex-ready-') as temp:
 p=Path(temp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
