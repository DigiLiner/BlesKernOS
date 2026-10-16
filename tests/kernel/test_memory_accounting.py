#!/usr/bin/env python3
"""Exercise production heap/system accounting against allocator snapshots."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'kernel/mm/memory.c').read_text()
heap = src[src.index('void mm_get_info('):src.index('\nbool mm_set_allocation_owner(')]
resident = src[src.index('void mm_resident_pages('):src.index('\nvoid mm_get_system_info(', src.index('void mm_resident_pages('))]
system = src[src.index('void mm_get_system_info('):src.index('\nstatic void mm_dump_locked(', src.index('void mm_get_system_info('))]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define U32_MAX_VALUE UINT32_MAX
#define MM_PAGE_SIZE 4096U
typedef struct heap_block {
    uint32_t magic; size_t size; bool free; uint32_t owner_process_id;
    struct heap_block *next, *prev;
} heap_block_t;
typedef struct {size_t total_bytes,used_bytes,free_bytes;uint32_t total_blocks,free_blocks,used_blocks;} heap_info_t;
typedef struct {size_t total_bytes,used_bytes,free_bytes,reserved_bytes;} system_memory_info_t;
static heap_block_t blocks[3], *heap_head, *heap_exec_reservation;
static uint32_t system_total_bytes,system_reserved_bytes,g_vm_frame_used,g_vm_frame_reserved_pages;
static int g_mm_vm_frame_lock;
static uint32_t mm_lock(void) {return 0;}
static void mm_unlock(uint32_t f) {(void)f;}
static bool mm_block_valid_locked(heap_block_t *b) {return b >= blocks && b < blocks+3;}
static uint32_t kspin_lock_irqsave(int *p) {(void)p;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f) {(void)p;(void)f;}
''' + heap + resident + system + r'''
int main(void) {
    blocks[0]=(heap_block_t){.size=4096,.free=true,.next=&blocks[1]};
    blocks[1]=(heap_block_t){.size=8192,.free=false,.next=&blocks[2]};
    blocks[2]=(heap_block_t){.size=4096,.free=true};
    heap_head=blocks;
    heap_info_t h; mm_get_info(&h);
    assert(h.used_bytes == 8192+3*sizeof(heap_block_t));
    assert(h.free_bytes == 8192 && h.free_blocks == 2 && h.used_blocks == 1);
    /* Pool capacity belongs to total RAM; only allocated frames are used.
     * Permanent pool bits are already charged in reserved, not twice. */
    system_total_bytes=128*1024*1024;
    system_reserved_bytes=36*1024*1024;
    g_vm_frame_used=g_vm_frame_reserved_pages=256;
    system_memory_info_t a,b;
    mm_get_system_info(&a);
    assert(a.used_bytes == h.used_bytes);
    assert(a.reserved_bytes == system_reserved_bytes);
    assert(a.used_bytes+a.free_bytes+a.reserved_bytes == a.total_bytes);
    g_vm_frame_used+=8;
    mm_get_system_info(&b);
    assert(b.used_bytes-a.used_bytes == 8*4096);
    assert(a.free_bytes-b.free_bytes == 8*4096);
    assert(b.free_bytes+b.used_bytes+b.reserved_bytes == b.total_bytes);
    g_vm_frame_used-=8;
    mm_get_system_info(&b);
    assert(a.free_bytes==b.free_bytes);
    /* Installed 3 GiB, identity/VM allocator manages only 1.5 GiB.
     * This is unavailable capacity, not 1.5 GiB allocated to the OS. */
    system_total_bytes=3U*1024U*1024U*1024U;
    system_reserved_bytes=1536U*1024U*1024U;
    mm_get_system_info(&b);
    assert(b.reserved_bytes==1536U*1024U*1024U);
    assert(b.used_bytes==h.used_bytes);
    assert(b.free_bytes==1536U*1024U*1024U-h.used_bytes);
    assert(b.free_bytes+b.used_bytes+b.reserved_bytes==b.total_bytes);
    system_total_bytes=1024; /* corrupt/inconsistent snapshot saturates */
    mm_get_system_info(&b);
    assert(!b.used_bytes && !b.free_bytes && b.reserved_bytes==system_reserved_bytes);
    uint32_t hp_before,hp_after,vp;
    mm_resident_pages(&hp_before,&vp);assert(vp==0);
    heap_exec_reservation=&blocks[1];
    mm_resident_pages(&hp_after,&vp);
    assert(hp_before==hp_after+2 && hp_after>=1 && vp==0);
    mm_get_info(&h);
    assert(h.used_bytes==3*sizeof(heap_block_t) && h.free_bytes==8192);
    system_total_bytes=128*1024*1024;
    system_reserved_bytes=36*1024*1024+8192;
    mm_get_system_info(&b);
    assert(b.used_bytes+8192==a.used_bytes && b.free_bytes==a.free_bytes);
    assert(b.used_bytes+b.free_bytes+b.reserved_bytes==b.total_bytes);
    puts("PASS heap metadata, free VM pool, frame allocation/free, reserved bits and saturation");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-memory-') as d:
    c=Path(d)/'test.c'; c.write_text(code)
    binary=Path(d)/'test'
    subprocess.run(['gcc','-O2','-Wall','-Werror',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
