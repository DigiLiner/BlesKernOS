#!/usr/bin/env python3
"""Exercise production mapping on OOM and overflowing virtual/physical ranges."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
src=(root/'kernel/mm/paging.c').read_text()
flags='\n'.join(line for line in src.splitlines() if line.startswith('#define PAGE_'))
bounds=src[src.index('static bool paging_range_bounds('):src.index('static bool paging_cpuid_available(')]
mapper=src[src.index('bool paging_process_map('):src.index('\nbool paging_process_map_auto(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define PAGING_PAGE_SIZE 4096U
''' + flags + r'''
typedef struct {uint32_t *directory;} paging_process_space_t;
static uint32_t directory[1024],tables[2][1024];
static paging_process_space_t space={directory};
static int g_paging_lock;
static bool fail;
static unsigned publications,waits;
static uint32_t kspin_lock_irqsave(int *p) {(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f) {(void)p;(void)f;}
static paging_process_space_t *paging_find_space(uint32_t id) {return id==1?&space:NULL;}
static uint32_t *paging_private_table_locked(paging_process_space_t *s,uint32_t pde) {
    (void)s;return pde>=2 || (pde==1 && fail)?NULL:tables[pde];
}
static uint32_t paging_publish_change(void) {return ++publications;}
static uint32_t paging_kick_generation(uint32_t id) {assert(id==1);return 2;}
static void paging_wait_generation(uint32_t g,uint32_t mask,uint32_t id) {
    assert(g==publications && mask==2 && id==1);++waits;
}
''' + bounds + mapper + r'''
int main(void) {
    tables[0][1023]=0xabcdef07;
    fail=true;
    assert(!paging_process_map(1,0x3ff000,0x100000,8192,true,true));
    assert(tables[0][1023]==0xabcdef07 && !publications);
    fail=false;
    assert(paging_process_map(1,0x3ff000,0x100000,8192,true,true));
    assert(tables[0][1023]==(0x100000|PAGE_PRESENT|PAGE_USER|PAGE_PRIVATE_PTE|PAGE_WRITABLE));
    assert((tables[1][0]&PAGE_FRAME_4K)==0x101000 && publications==1 && waits==1);
    assert(!paging_process_map(1,0xfffff000,0x100000,4096,true,true));
    assert(!paging_process_map(1,0x400000,0xfffff000,8192,true,true));
    assert(!paging_process_map(1,0x400001,0x100000,4096,true,true));
    assert(!paging_process_map(2,0x400000,0x100000,4096,true,true));
    assert(publications==1);
    puts("PASS OOM preserves existing PTEs; overflow/alignment rejected; remaps publish TLB changes");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-map-') as d:
    c=Path(d)/'test.c';c.write_text(code)
    binary=Path(d)/'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
