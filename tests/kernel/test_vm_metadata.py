#!/usr/bin/env python3
"""Production VMA metadata growth: supervisor allocation, OOM and release."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'kernel/api/syscall.c')
args = parser.parse_args()
source = args.source.read_text()
start = source.index('static uint32_t sys_vm_map_pages(') if 'static uint32_t sys_vm_map_pages(' in source else source.index('static bool sys_vm_maps_reserve(')
functions = source[start:source.index('static syscall_vm_map_t *sys_vm_free_map_slot(')]
types = source[source.index('typedef struct {\n    uint32_t backing;'):source.index('typedef struct {\n    int fds[')]
limit = int(re.search(r'#define SYSCALL_VM_MAP_LIMIT\s+(\d+)U', source)[1])
assert limit >= 4096, 'real games need more than 1024 VMA descriptors'
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void vfs_shared_mapping_t;
#define SYSCALL_VM_MAP_INITIAL 64U
#define SYSCALL_VM_MAP_LIMIT @LIMIT@U
#define PAGING_PAGE_SIZE 4096U
#define MM_SUBSYSTEM_VM 3U
#define kmemset memset
#define kmemcpy memcpy
static unsigned char frames[256][4096] __attribute__((aligned(4096)));
static bool used[256], oom;
static unsigned allocations, releases;
static unsigned live_pages(void) {
    unsigned count=0;for(unsigned i=0;i<256;i++)count+=used[i];return count;
}
static bool supervisor(const void *p) {
    return (uintptr_t)p>=(uintptr_t)frames && (uintptr_t)p<(uintptr_t)(frames+256);
}
static __attribute__((unused)) bool mm_vm_frame_alloc(uint32_t count,uint32_t *out) {
    *out=0;if(oom)return false;
    for(unsigned i=0;i+count<=256;i++) {
        bool free_run=true;for(unsigned j=0;j<count;j++)if(used[i+j])free_run=false;
        if(!free_run)continue;
        for(unsigned j=0;j<count;j++)used[i+j]=true;
        /* Deliberately poison new memory: metadata must initialize its slots. */
        memset(frames[i],0xa5,count*4096);
        *out=(uint32_t)(uintptr_t)frames[i];allocations++;return true;
    }
    return false;
}
static __attribute__((unused)) void mm_vm_frame_free(uint32_t address,uint32_t count) {
    assert(supervisor((void *)(uintptr_t)address) && !(address&4095U));
    unsigned first=(address-(uint32_t)(uintptr_t)frames)/4096;
    assert(count && first+count<=256);
    for(unsigned j=0;j<count;j++){assert(used[first+j]);used[first+j]=false;}
    releases++;
}
static __attribute__((unused)) void *krealloc(void *p,uint32_t n){return oom?NULL:realloc(p,n);}
static __attribute__((unused)) bool mm_set_allocation_owner(void *p,uint32_t id){(void)p;(void)id;return true;}
static __attribute__((unused)) bool mm_set_allocation_subsystem(void *p,uint32_t id){(void)p;(void)id;return true;}
''' + types + functions + r'''
int main(void) {
    syscall_vm_state_t vm={0},child={0};
    assert(!sys_vm_maps_reserve(NULL,1));
    assert(!sys_vm_maps_reserve(&vm,SYSCALL_VM_MAP_LIMIT+1U));
    assert(!allocations && !vm.maps);
    oom=true;assert(!sys_vm_maps_reserve(&vm,64));
    assert(!vm.maps && !vm.map_capacity && !live_pages());
    oom=false;assert(sys_vm_maps_reserve(&vm,1));
    if(!supervisor(vm.maps))fputs("FAIL VMA metadata allocated in legacy user-visible heap\n",stderr);
    assert(supervisor(vm.maps) && vm.map_capacity==64 && live_pages()==1);
    for(unsigned i=0;i<64;i++)assert(!vm.maps[i].address && !vm.maps[i].backing);
    vm.maps[0]=(syscall_vm_map_t){.address=0x60000000,.length=4096,.protection=3,.flags=0x22};
    syscall_vm_map_t *previous=vm.maps;
    oom=true;assert(!sys_vm_maps_reserve(&vm,65));
    assert(vm.maps==previous && vm.map_capacity==64 && vm.maps[0].length==4096 && live_pages()==1);
    oom=false;assert(sys_vm_maps_reserve(&vm,65));
    assert(vm.map_capacity==128 && supervisor(vm.maps) && vm.maps[0].address==0x60000000);
    for(unsigned i=64;i<128;i++)assert(!vm.maps[i].address && !vm.maps[i].shared);
    assert(sys_vm_maps_reserve(&vm,129) && vm.map_capacity==256 && live_pages()==2);
    assert(sys_vm_maps_reserve(&child,vm.map_capacity));
    memcpy(child.maps,vm.maps,vm.map_capacity*sizeof(*vm.maps));
    child.maps[0].protection=1;
    assert(vm.maps[0].protection==3 && child.maps!=vm.maps && live_pages()==4);
    assert(sys_vm_maps_reserve(&vm,1024) && vm.map_capacity==1024 && live_pages()==8);
    assert(sys_vm_maps_reserve(&vm,2049) && vm.map_capacity==4096);
    assert(vm.maps[0].address==0x60000000 && vm.maps[0].protection==3);
    assert(sys_vm_maps_reserve(&vm,SYSCALL_VM_MAP_LIMIT));
    assert(vm.map_capacity==SYSCALL_VM_MAP_LIMIT);
    for(unsigned i=1024;i<vm.map_capacity;i++)assert(!vm.maps[i].address);
    unsigned before=allocations;
    assert(sys_vm_maps_reserve(&vm,64) && allocations==before);
''' + (r'''
    sys_vm_maps_release(&child);sys_vm_maps_release(&vm);
    assert(!child.maps && !child.map_capacity && !vm.maps && !vm.map_capacity);
    sys_vm_maps_release(&vm);sys_vm_maps_release(NULL);
    assert(!live_pages() && allocations==releases);
''' if 'static void sys_vm_maps_release(' in source else '') + r'''
    puts("PASS supervisor VMA metadata, initialization, growth, OOM atomicity and independent child tables");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vm-metadata-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    c.write_text(code.replace('@LIMIT@',str(limit)))
    subprocess.run(['gcc', '-m32', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
