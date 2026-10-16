#!/usr/bin/env python3
"""Signal frame materialization may block; the final IRET handoff must stay atomic."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_signal.inc').read_text()
body=s[s.index('registers_t *task_linux_signal_return'):s.index('registers_t *task_linux_signal_fault')]
body=body.replace('__asm__ volatile("sti" ::: "memory");','irq_enabled=true;').replace('__asm__ volatile("cli" ::: "memory");','irq_enabled=false;')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define TASK_ZOMBIE 4
#define TASK_RUNNING 2
#define BK_EAGAIN 11
#define kprintf(...) ((void)0)
typedef struct {uint32_t cs,eax;} registers_t;
typedef struct {uint32_t sig;int code;uint32_t data;} linux_pending;
typedef struct {uint32_t futex_key;registers_t *futex_return_frame;bool futex_return_pending,futex_return_timed;uint64_t mask;linux_pending queue[32];} linux_signals;
typedef struct {bool linux_abi;int state;linux_signals *linux_signals;} task_t;
static linux_signals signals;
static task_t task={true,TASK_RUNNING,&signals};
static bool irq_enabled,locked,fail_delivery;
static unsigned delivered,scheduled;
static int g_task_table_lock;
static task_t *task_current_entry(void){return &task;}
static uint32_t kspin_lock_irqsave(int *lock){(void)lock;assert(!locked);locked=true;return irq_enabled;}
static void kspin_unlock_irqrestore(int *lock,uint32_t f){(void)lock;assert(locked);locked=false;irq_enabled=f;}
static registers_t *task_schedule(registers_t *r){assert(!irq_enabled && !locked);scheduled++;return r;}
static bool linux_signal_deliver(registers_t *r,linux_pending p){(void)r;assert(irq_enabled && !locked && p.sig==10);delivered++;return !fail_delivery;}
'''+body+r'''
int main(void){registers_t r={.cs=3};
 signals.queue[0]=(linux_pending){10,2,123};signals.queue[1]=(linux_pending){12,1,99};
 assert(task_linux_signal_return(&r)==&r && delivered==1 && !irq_enabled && signals.queue[0].sig==12);
 signals.mask=1ULL<<11;assert(task_linux_signal_return(&r)==&r && delivered==1 && !irq_enabled);
 signals.queue[0]=(linux_pending){10,2,123};fail_delivery=true;
 assert(task_linux_signal_return(&r)==&r && delivered==2 && scheduled==1 && !irq_enabled);
 task.state=TASK_ZOMBIE;assert(task_linux_signal_return(&r)==&r && scheduled==2 && !irq_enabled);
 task.state=TASK_RUNNING;r.cs=0;assert(task_linux_signal_return(&r)==&r && scheduled==2 && !irq_enabled);
 puts("PASS signal frame work permits blocking outside spinlock; masked signals, failed delivery, zombie/kernel returns and final scheduler handoff keep IRQs disabled");}
'''
with tempfile.TemporaryDirectory(prefix='bk-sigreturn-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
