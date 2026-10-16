#!/usr/bin/env python3
"""Exercise the real hot mapping lookup and VFS guard against holes and reuse."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/pager.c').read_text()
lookup=s[s.index('static bool region_contains'):s.index('static void *resident_alloc')]
lookup=lookup.replace('return r && r->pid==pid', 'visits++; return r && r->pid==pid')
guard=s[s.index('static bool file_guard'):s.index('static bool reclaim_clean')]
structs=s[s.index('typedef struct {uint32_t frame'):s.index('static kmutex_t lock')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
typedef struct file_source {int unused;} file_source_t;
typedef int kmutex_t;
static kmutex_t lock;
static unsigned visits,depth;
#define KDOMAIN_VFS 2
static void kmutex_lock(kmutex_t *p){(void)p;}
static void kmutex_unlock(kmutex_t *p){(void)p;}
static void kernel_domains_enter(unsigned d){assert(d==2);depth++;}
static void kernel_domains_exit(unsigned d){assert(d==2 && depth);depth--;}
'''+structs+lookup+guard+r'''
int main(void){
 pager_region_t *target=NULL;
 for(unsigned i=0;i<1024;i++){
  pager_region_t *r=calloc(1,sizeof(*r)+3*sizeof(pager_page_t));assert(r);
  r->pid=1;r->address=0x40000000+i*65536;r->count=3;
  for(unsigned j=0;j<3;j++)r->pages[j].alive=true;
  region_publish(r);if(!i)target=r;
 }
 assert(find_region(1,target->address)==target);
 unsigned before=visits;
 for(unsigned i=0;i<10000;i++)assert(find(1,target->address+4)==&target->pages[0]);
 assert(visits-before==10000); /* One region check instead of 1024 per lookup. */
 assert(!find(1,0x10000000));before=visits;
 for(unsigned i=0;i<10000;i++)assert(!find(1,0x10000000+i*4096U));
 assert(visits-before<=10000); /* An entire non-pager buffer uses one cached gap. */
 pager_region_t *inserted=calloc(1,sizeof(*inserted)+sizeof(pager_page_t));assert(inserted);
 inserted->pid=1;inserted->address=0x10001000;inserted->count=1;inserted->pages[0].alive=true;
 region_publish(inserted);assert(find_region(1,inserted->address)==inserted);
 region_forget(inserted);regions=inserted->next;free(inserted);
 assert(!find(1,0x10001000));assert(!find(65,0x10001000));
 assert(!find(65,target->address)); /* Same bucket, different process. */
 assert(find(1,target->address+4096));target->pages[1].alive=false;
 assert(!find(1,target->address+4096));
 pager_region_t *file=calloc(1,sizeof(*file)+sizeof(pager_page_t));assert(file);
 file->pid=1;file->address=target->address+4096;file->count=1;
 file->pages[0].alive=true;file_source_t inode;file->file=&inode;
 region_publish(file);
 assert(!file_guard(1,0x10000000,8192) && !depth);
 /* A cached gap must not bypass a file mapping at the end of a buffer. */
 bool crossing=file_guard(1,file->address-4096,8192);
 assert(crossing && depth==1);file_unguard(crossing);
 assert(find_region(1,file->address)==file);
 assert(!file_guard(1,target->address,16) && !depth);
 bool held=file_guard(1,target->address,8192);assert(held && depth==1);file_unguard(held);
 assert(file_guard(1,file->address,16));file_unguard(true);
 assert(!file_guard(1,target->address+8192,16) && !depth);
 assert(find_region(1,file->address)==file);
 region_forget(file);regions=file->next;free(file);
 assert(!find(1,target->address+4096));
 assert(find_region(1,target->address)==target);
 region_forget(target);
 for(unsigned i=0;i<64;i++)assert(region_cache[i]!=target);
 while(regions){pager_region_t *r=regions;regions=r->next;region_forget(r);free(r);}
 for(unsigned i=0;i<64;i++)assert(!region_cache[i]);
 puts("PASS 1024 mappings: 10000 hot hits and cross-page gap hits; new mapping invalidates misses; PID collisions, partial unmap, file in a hole, VFS locking and freed-pointer invalidation");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-pager-lookup-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
