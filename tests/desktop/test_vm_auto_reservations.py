#!/usr/bin/env python3
"""Automatic mmap must respect metadata-only Wine address reservations."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
paging = (root / 'kernel/mm/paging.c').read_text()
syscall = (root / 'kernel/api/syscall.c').read_text()
find = paging[paging.index('static uint32_t paging_space_find_user_range('):paging.index('\nstatic void paging_invlpg(')]
skip = syscall[syscall.index('static uint32_t sys_vm_auto_skip('):syscall.index('\nstatic uint32_t sys_vm_brk(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define PAGING_PAGE_SIZE 4096U
#define PAGE_FRAME_4K 0xfffff000U
#define PAGING_USER_VM_START 0x60000000U
#define PAGING_USER_VM_END 0x78000000U
typedef uint32_t (*paging_range_skip_t)(void *,uint32_t,uint32_t);
typedef struct { uint32_t user_vm_hint; } paging_process_space_t;
typedef struct { uint32_t backing,address,length; } syscall_vm_map_t;
typedef struct { syscall_vm_map_t *maps; uint32_t map_capacity,brk_base,brk_capacity; } syscall_vm_state_t;
static unsigned probes;
static bool paging_space_user_range_free(paging_process_space_t *s,uint32_t a,uint32_t n) {
    (void)s;(void)a;(void)n; ++probes; return true;
}
''' + skip + find + r'''
int main(void) {
    syscall_vm_map_t maps[]={{0,0x60000000,0x10000000},{0,0x71000000,0x1000000}};
    syscall_vm_state_t vm={maps,2,0x70000000,0x20000};
    paging_process_space_t space={0x60000000};
    /* No PTEs exist, yet reservations and the native heap remain owned. */
    assert(paging_space_find_user_range(&space,0,4096,sys_vm_auto_skip,&vm)==0x70020000);
    assert(probes==1); /* Skip a 256 MiB reservation in one step. */
    assert(paging_space_find_user_range(&space,0x60010000,4096,sys_vm_auto_skip,&vm)==0x70020000);
    assert(paging_space_find_user_range(&space,0x72000000,4096,sys_vm_auto_skip,&vm)==0x72000000);
    /* A candidate whose end crosses a reservation cannot be selected. */
    assert(paging_space_find_user_range(&space,0x70fff000,8192,sys_vm_auto_skip,&vm)==0x70020000);
    space.user_vm_hint=0x77fff000;
    assert(paging_space_find_user_range(&space,0,8192,sys_vm_auto_skip,&vm)==0x70020000);
    maps[0].length=0x18000000;
    assert(!paging_space_find_user_range(&space,0,4096,sys_vm_auto_skip,&vm));
    /* Legacy paging clients without VM metadata retain their allocator. */
    assert(paging_space_find_user_range(&space,0x60000000,4096,0,0)==0x60000000);
    puts("PASS automatic mmap skips reservations, brk, crossing hints and wraps; full arena returns failure");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vm-reserve-') as directory:
    c = Path(directory) / 'test.c'; c.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
