#!/usr/bin/env python3
"""Bulk pager protection preserves holes, pins, ownership and COW groups."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/pager.c').read_text()
functions=s[s.index('bool pager_protect_mapped('):s.index('bool pager_reset(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef struct {uint32_t frame,slot,pins,source_frame;bool alive;} pager_page_t;
typedef struct region {struct region *next;uint32_t pid,address,count;void *file;pager_page_t pages[8];} pager_region_t;
static pager_region_t *regions;
static int lock;
static unsigned locks,changes,cow_changes;
static bool last_user,last_write;
static uint32_t affected[8],cow_mask;
static void kmutex_lock(int *l){(void)l;assert(!locks++);}
static void kmutex_unlock(int *l){(void)l;assert(locks--==1);}
static uint32_t paging_process_owner(uint32_t pid){return pid;}
static bool apply(uint32_t pid,uint32_t address,uint32_t bytes,bool user,bool write,bool cow){
 assert(locks==1 && pid==1 && address>=0x40000000 && bytes);
 last_user=user;last_write=write;
 changes++;cow_changes+=cow;
 for(uint32_t a=address;a<address+bytes;a+=4096){unsigned i=(a-0x40000000)/4096;assert(i<8);affected[i]++;if(cow)cow_mask|=1U<<i;}
 return true;
}
static bool paging_process_access(uint32_t pid,uint32_t a,uint32_t n,bool u,bool w){return apply(pid,a,n,u,w,false);}
static bool paging_process_cow_access(uint32_t pid,uint32_t a,uint32_t n,bool u,bool w){return apply(pid,a,n,u,w,true);}
'''+functions+r'''
int main(void){
 pager_region_t old={.pid=1,.address=0x40000000,.count=8};regions=&old;
 for(unsigned i=0;i<8;i++)old.pages[i].alive=true;
 old.pages[1].source_frame=0x1000;old.pages[2].slot=1;
 assert(pager_protect(1,old.address,32768,3));assert(changes==3 && cow_changes==1 && cow_mask==2);
 for(unsigned i=0;i<8;i++)assert(affected[i]==1);
 unsigned before=changes;old.pages[7].pins=1;
 assert(!pager_protect(1,old.address,32768,3) && changes==before && !locks);old.pages[7].pins=0;
 old.pages[4].alive=false;
 assert(!pager_protect(1,old.address,32768,3) && changes==before);
 assert(pager_protect_mapped(1,old.address,32768,3,28672));assert(affected[4]==1);
 before=changes;assert(!pager_protect_mapped(1,old.address,32768,3,24576) && changes==before);
 pager_region_t fresh={.next=&old,.pid=1,.address=0x40004000,.count=1,.file=&old};fresh.pages[0].alive=true;regions=&fresh;
 assert(pager_protect(1,old.address,32768,3));assert(cow_mask&(1U<<4));
 fresh.pid=2;before=changes;assert(!pager_protect(1,old.address,32768,3) && changes==before);
 assert(!pager_protect(1,0xfffff000,8192,3));assert(!pager_protect(1,old.address+1,4096,3));
 assert(!pager_protect_mapped(1,old.address,4096,3,8192));assert(!pager_protect(1,old.address,4096,8));
 fresh.pid=1;assert(pager_protect(1,old.address,32768,1) && last_user && !last_write);
 assert(pager_protect(1,old.address,32768,0) && !last_user && !last_write);
 assert(pager_protect(1,old.address,0,3) && !locks);
 puts("PASS bulk pager protection: exact coverage, holes/reuse, pins before mutations, COW/anonymous grouping, PID isolation and overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-pager-protect-') as folder:
 c=Path(folder)/'test.c';c.write_text(code);binary=Path(folder)/'test'
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
