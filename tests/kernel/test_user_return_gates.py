#!/usr/bin/env python3
"""Native exit/upcall gates occupy one dedicated, read-only user code page."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2];source=(root/'kernel/sched/task.c').read_text()
start=source.index('static bool task_user_gates_init(');end=source.index('\nvoid task_init(',start)
code='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define PAGING_PAGE_SIZE 4096U
#define TASK_USER_GATE_SIZE 32U
#define SYS_EXIT 0U
#define SYS_YIELD 3U
#define SYS_UPCALL_RETURN 25U
#define kmemset memset
static uint8_t g_user_gate_page[4096] __attribute__((aligned(4096)));
static uint8_t *g_user_process_exit_gate,*g_user_upcall_return_gate;
static int permit,calls;
static bool paging_set_range(uint32_t address,uint32_t size,bool present,bool user,bool writable){
 assert(address==(uintptr_t)g_user_gate_page && !(address&4095U) && size==4096);
 assert(present && user && !writable);assert(!g_user_process_exit_gate && !g_user_upcall_return_gate);calls++;return permit;
}
'''+source[start:end]+'''
int main(void){
 assert(!task_user_gates_init() && calls==1 && !g_user_process_exit_gate);
 permit=1;assert(task_user_gates_init() && calls==2);
 assert(g_user_process_exit_gate==g_user_gate_page && g_user_upcall_return_gate==g_user_gate_page+32);
 assert(g_user_gate_page[0]==0x31 && g_user_gate_page[7]==0xcd && g_user_gate_page[8]==0x80);
 assert(g_user_gate_page[32]==0x89 && g_user_gate_page[39]==0xcd && g_user_gate_page[40]==0x80);
 assert(*(uint32_t *)(g_user_gate_page+3)==SYS_EXIT && *(uint32_t *)(g_user_gate_page+35)==SYS_UPCALL_RETURN);
 assert(task_user_gates_init() && calls==2);
 puts("PASS dedicated RO native return gates: page alignment, protected publication, unchanged exit/upcall ABI, initialization failure and idempotence");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-user-gates-') as folder:
 path=Path(folder)/'test.c';path.write_text(code);exe=Path(folder)/'test'
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
# Present/write user faults are gated by actual COW permission, not personality.
idt=(root/'kernel/arch/x86/idt.c').read_text()
assert re.search(r'\(regs->err_code & 3U\) == 3U &&\s+paging_cow_write_pending\(fault_address\)',idt)
