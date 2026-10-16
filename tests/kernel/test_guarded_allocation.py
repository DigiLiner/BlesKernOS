#!/usr/bin/env python3
"""Exercise real guarded allocation, including zeroed non-stack IPC storage."""
import subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/mm/memory.c').read_text()
body=source[source.index('void *mm_alloc_guarded_stack('):source.index('/* Forked identity-addressed ELF/stack views')]
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGING_PAGE_SIZE 4096U
#define MM_SUBSYSTEM_PROCESS 4
static unsigned live,calls,fail_call;
static bool fail_alloc;
static void *kmalloc(size_t size) {
    if(fail_alloc)return NULL;
    void *p=malloc(size);assert(p);memset(p,0xCD,size);live++;return p;
}
static void kfree(void *p){assert(live);live--;free(p);}
#define kmemset memset
static bool mm_set_allocation_owner(void *p,unsigned owner){return p && !owner;}
static bool mm_set_allocation_subsystem(void *p,unsigned subsystem){return p && subsystem==MM_SUBSYSTEM_PROCESS;}
static bool paging_set_range(uint32_t address,uint32_t size,bool present,bool user,bool write) {
    assert(!(address&4095U) && !(size&4095U));
    if(!present)assert(!user && !write);
    calls++;return calls!=fail_call;
}
'''
checks=r'''
int main(void) {
    for(unsigned user=0;user<2;user++) {
        void *allocation=(void *)1;uint32_t size=1;
        unsigned char *buffer=mm_alloc_guarded_stack(17001,user,&allocation,&size);
        assert(buffer && allocation && size==20480 && !((uintptr_t)buffer&4095));
        /* The kernel uses this allocator for Unix namespaces and inode tables,
           whose unused entries must be zero even when they aren't user stacks. */
        for(unsigned i=0;i<size;i++)assert(!buffer[i]);
        memset(buffer,0xEF,size);mm_free_guarded_stack(allocation,buffer,size);assert(!live);
    }
    for(unsigned failure=1;failure<=3;failure++) {
        calls=0;fail_call=failure;void *allocation=(void *)1;uint32_t size=1;
        assert(!mm_alloc_guarded_stack(4096,false,&allocation,&size));
        assert(!allocation && !size && !live);
    }
    fail_call=0;fail_alloc=true;void *allocation=(void *)1;uint32_t size=1;
    assert(!mm_alloc_guarded_stack(65536,false,&allocation,&size));
    assert(!allocation && !size && !live);
    assert(!mm_alloc_guarded_stack(0xffffffffU,false,NULL,NULL));
    puts("PASS guarded IPC/user storage starts zero; aligned guards, allocation failure and PTE rollback release all storage");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-guarded-test-') as temp:
    c=Path(temp)/'test.c';binary=Path(temp)/'test'
    c.write_text(harness+body+checks)
    subprocess.run(['gcc','-m32','-std=c11','-O2',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
