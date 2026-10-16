#!/usr/bin/env python3
"""rt_sigaction copies pinned output without sleeping under the task spinlock."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linux/i386_signal.inc').read_text()
branch=s.split('    if (nr==174) {',1)[1].split('    } else if (nr==175)',1)[0]
code=r'''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define BK_EINVAL 22
#define BK_EFAULT 14
#define LINUX_UNBLOCKABLE 0x100U
typedef struct {uint32_t handler,flags,restorer;uint64_t mask;} linux_action;
typedef struct {uint32_t ebx,ecx,edx,esi;} registers_t;
static struct {linux_action actions[64];} shared;
static struct {__typeof__(shared) *shared;} signals={&shared};
static int g_task_table_lock;static unsigned pins;static bool fail_pin,fail_access;
static uint32_t task_current_process_id(void){return 7;}
static bool copy_from_user(void *d,const void *s,uint32_t n){assert(!g_task_table_lock);memcpy(d,s,n);return true;}
static bool pager_pin(uint32_t p,uint32_t a,uint32_t n,bool w){assert(!g_task_table_lock && p==7 && a && n==sizeof(linux_action) && w);if(fail_pin)return false;pins++;return true;}
static void pager_unpin(uint32_t p,uint32_t a,uint32_t n){assert(!g_task_table_lock && p==7 && a && n==sizeof(linux_action) && pins);pins--;}
static bool user_access_ok(void *p,uint32_t n,bool w){assert(!g_task_table_lock && p && n && w);return !fail_access;}
static uint32_t kspin_lock_irqsave(int *p){assert(!*p);*p=1;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)f;assert(*p);*p=0;}
#define kmemcpy memcpy
static int test(registers_t *r){uint32_t flags;int result=0;__typeof__(signals) *s=&signals;
'''+branch+r'''
return result;}
int main(void){linux_action input={99,3,11,0x1ff},output={0};registers_t r={2,(uint32_t)&input,(uint32_t)&output,8};shared.actions[1].handler=42;
 assert(test(&r)==0 && output.handler==42 && shared.actions[1].handler==99 && shared.actions[1].mask==0xff && !pins);
 fail_pin=true;input.handler=101;assert(test(&r)==-BK_EFAULT && shared.actions[1].handler==99 && !pins);fail_pin=false;fail_access=true;
 assert(test(&r)==-BK_EFAULT && shared.actions[1].handler==99 && !pins);fail_access=false;r.ecx=0;assert(test(&r)==0 && output.handler==99 && !pins);
 puts("PASS old-action copy, atomic update, pager outside spinlock, pin/access errors and balanced release");}
'''
with tempfile.TemporaryDirectory(prefix='bk-sigcopy-') as directory:
 d=Path(directory);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
