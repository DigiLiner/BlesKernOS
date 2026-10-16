#!/usr/bin/env python3
"""Dense Wine heap commits protect paged VMAs once and commit only reservations."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/api/syscall.c').read_text();start=s.index('    uint32_t covered_bytes = 0U, intersections = 0U')
end=s.index('\n    for(uint32_t i=0;i<vm->map_capacity;i++) {',start)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define BK_ENOMEM 12
#define VM_MAP_PAGED 0x40000000U
typedef struct {uint32_t backing,address,length,protection,flags;void *shared;} syscall_vm_map_t;
typedef struct {syscall_vm_map_t *maps;uint32_t map_capacity;} syscall_vm_state_t;
static unsigned bulk,commits,backed;
static uint32_t expected,first,last;
static bool fail_bulk;
static bool pager_protect_mapped(uint32_t pid,uint32_t a,uint32_t n,uint32_t p,uint32_t bytes){assert(pid==1 && a==first && a+n==last && p==3 && bytes==expected);bulk++;return !fail_bulk;}
static bool paging_process_cow_access(uint32_t pid,uint32_t a,uint32_t n,bool u,bool w){assert(pid==1 && a && n && u && w);backed++;return true;}
#define paging_process_access paging_process_cow_access
static int32_t sys_vm_mprotect(uint32_t a,uint32_t n,uint32_t p){assert(a==last-4096 && n==4096 && p==3);commits++;return 0;}
static int32_t protect(syscall_vm_state_t *vm,uint32_t address,uint32_t rounded_end,uint32_t protection){
 uint32_t process_id=1;
'''+s[start:end]+r'''
 return -BK_ENOMEM;
}
int main(void){
 syscall_vm_map_t *maps=calloc(2049,sizeof(*maps));assert(maps);syscall_vm_state_t vm={maps,2049};
 first=0x40000000;last=first+2049U*4096;expected=2048U*4096;
 for(unsigned i=0;i<2049;i++)maps[i]=(syscall_vm_map_t){.address=first+i*4096,.length=4096,.flags=VM_MAP_PAGED|0x22};
 maps[2048].flags=0x22;
 assert(!protect(&vm,first,last,3) && bulk==1 && commits==1 && !backed);
 for(unsigned i=0;i<2048;i++)assert(maps[i].protection==3);
 bulk=commits=backed=0;maps[2048].backing=0x200000;assert(!protect(&vm,first,last,3) && bulk==1 && !commits && backed==1);
 maps[2048].shared=&vm;maps[2048].flags=0x80000002U;bulk=backed=0;
 assert(protect(&vm,first,last,3)==-13 && !bulk && !backed);
 maps[2048].shared=NULL;maps[2048].flags=0x22;fail_bulk=true;
 assert(protect(&vm,first,last,3)==-BK_ENOMEM && !backed && !commits);
 free(maps);puts("PASS 2048 paged commits: one bulk protection, no paged recursion, reservation-only commit, backing/COW permissions and failed preflight");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-vm-spans-') as folder:
 c=Path(folder)/'test.c';c.write_text(code);binary=Path(folder)/'test'
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
