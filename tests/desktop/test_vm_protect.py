#!/usr/bin/env python3
"""Validate real page protection updates and redundant TLB invalidations."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/paging.c').read_text()
a = source.index('static bool paging_process_access_mode(')
b = source.index('\nbool paging_process_access(', a)
flags = '\n'.join(line for line in source.splitlines() if line.startswith('#define PAGE_'))
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define PAGING_PAGE_SIZE 4096U
''' + flags + r'''
typedef struct { uint32_t *table[1]; uint32_t table_count, pde_index[1]; } paging_process_space_t;
static uint32_t entries[1024];
static paging_process_space_t space = {.table={entries}, .table_count=1, .pde_index={1}};
static int g_paging_lock;
static unsigned publications, kicks, waits;
static uint32_t kspin_lock_irqsave(int *p) { (void)p; return 0; }
static void kspin_unlock_irqrestore(int *p, uint32_t f) { (void)p; (void)f; }
static paging_process_space_t *paging_find_space(uint32_t id) { return id == 1 ? &space : NULL; }
static uint32_t paging_publish_change(void) { return ++publications; }
static uint32_t paging_kick_generation(uint32_t id) { assert(id == 1); ++kicks; return 1; }
static void paging_wait_generation(uint32_t gen,uint32_t mask,uint32_t id) {
    assert(gen == publications && mask == 1 && id == 1); ++waits;
}
''' + source[source.index('static bool paging_range_bounds('):source.index('static bool paging_cpuid_available(')] + source[a:b] + r'''
int main(void) {
    entries[0] = 0x12345000 | PAGE_PRESENT | PAGE_PRIVATE_PTE | PAGE_USER | PAGE_WRITABLE;
    entries[1] = 0x12346000 | PAGE_PRESENT | PAGE_PRIVATE_PTE | PAGE_USER | PAGE_WRITABLE;
    for (unsigned i = 0; i < 1000; ++i)
        assert(paging_process_access_mode(1,0x400000,8192,true,true,false));
    assert(!publications && !kicks && !waits);
    /* Read-only, inaccessible and COW transitions must still invalidate. */
    assert(paging_process_access_mode(1,0x400000,8192,true,false,false));
    assert(publications == 1 && waits == 1 && !(entries[0] & PAGE_WRITABLE));
    assert((entries[1] & PAGE_FRAME_4K) == 0x12346000);
    assert(paging_process_access_mode(1,0x400000,8192,false,false,false));
    assert(publications == 2 && !(entries[0] & PAGE_USER) && (entries[0] & PAGE_USER_NONE));
    assert(paging_process_access_mode(1,0x400000,8192,true,true,true));
    assert(publications == 3 && (entries[0] & PAGE_COPY_ON_WRITE) && !(entries[0] & PAGE_WRITABLE));
    assert(paging_process_access_mode(1,0x400000,8192,true,true,true));
    assert(publications == 3);
    assert(!paging_process_access_mode(1,0x400000,12288,true,false,false));
    assert(publications == 3 && (entries[0] & PAGE_COPY_ON_WRITE));
    assert(!paging_process_access_mode(2,0x400000,4096,true,false,false));
    /* An unaligned span crossing a boundary protects BOTH pages. */
    assert(paging_process_access_mode(1,0x400fff,2,true,false,false));
    assert(!(entries[0] & PAGE_COPY_ON_WRITE) && !(entries[1] & PAGE_COPY_ON_WRITE));
    unsigned before = publications;
    assert(!paging_process_access_mode(1,0xfffff000,4096,true,true,false));
    assert(!paging_process_access_mode(1,0xfffffffe,4,true,true,false));
    assert(publications == before);
    /* Inherited supervisor and user aliases are never process-owned. */
    entries[1] = 0x12346000 | PAGE_PRESENT | PAGE_WRITABLE;
    uint32_t original = entries[0];
    assert(!paging_process_access_mode(1,0x400000,8192,true,true,false));
    assert(entries[0] == original && !(entries[1] & PAGE_USER));
    entries[1] |= PAGE_USER;
    assert(!paging_process_access_mode(1,0x401000,4096,true,true,false));
    entries[1] = PAGE_PRESENT | PAGE_PRIVATE_PTE; /* metadata guard */
    assert(!paging_process_access_mode(1,0x401000,4096,true,true,false));
    assert(publications == before);
    puts("PASS unchanged PTEs avoid shootdowns; read-only, PROT_NONE and COW invalidate; holes preserve pages");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-protect-') as directory:
    c = Path(directory) / 'test.c'; c.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
