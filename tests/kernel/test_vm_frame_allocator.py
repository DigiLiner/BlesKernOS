#!/usr/bin/env python3
"""Exercise production physical-frame allocation and bounded sequential scan cost."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/memory.c').read_text()
part=s[s.index('bool mm_vm_frame_alloc('):s.index('uint32_t mm_vm_frame_total_pages')]
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define MM_PAGE_SIZE 4096U
static uint32_t g_vm_frame_start,g_vm_frame_end,g_vm_frame_pages=256,g_vm_frame_used,g_vm_frame_hint;
static uint32_t g_mm_vm_frame_lock;
static bool bits[256];static unsigned probes;
static unsigned kspin_lock_irqsave(uint32_t *p){(void)p;return 0;}
static void kspin_unlock_irqrestore(uint32_t *p,unsigned f){(void)p;(void)f;}
static bool mm_vm_bit_test(unsigned n){assert(n<256);probes++;return bits[n];}
static void mm_vm_bit_set(unsigned n){assert(n<256 && !bits[n]);bits[n]=true;}
static void mm_vm_bit_clear(unsigned n){assert(n<256);bits[n]=false;}
static bool mm_win32_image_reserved(unsigned n){return n==g_vm_frame_start+100*4096;}
#define kmemset memset
'''+part+r'''
int main(void){
 void *pool=mmap(NULL,256*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(pool!=MAP_FAILED && (uintptr_t)pool<UINT32_MAX-256*4096);
 g_vm_frame_start=(uint32_t)(uintptr_t)pool;g_vm_frame_end=g_vm_frame_start+256*4096;
 bits[100]=true;g_vm_frame_used=1;
 uint32_t addresses[255];
 for(unsigned i=0;i<255;i++){
  assert(mm_vm_frame_alloc(1,&addresses[i]));assert(addresses[i]!=g_vm_frame_start+100*4096);
  for(unsigned j=0;j<4096;j++)assert(!((unsigned char *)(uintptr_t)addresses[i])[j]);
  memset((void *)(uintptr_t)addresses[i],0xa5,4096);
 }
 assert(probes<=256);uint32_t a;assert(!mm_vm_frame_alloc(1,&a) && !a);
 for(unsigned i=0;i<255;i++)mm_vm_frame_free(addresses[i],1);
 assert(g_vm_frame_used==1 && bits[100]);
 /* The only suitable contiguous run spans the hint. */
 memset(bits,1,sizeof bits);g_vm_frame_used=256;
 for(unsigned i=20;i<40;i++){bits[i]=false;g_vm_frame_used--;}
 g_vm_frame_hint=30;assert(mm_vm_frame_alloc(20,&a) && a==g_vm_frame_start+20*4096);
 mm_vm_frame_free(a,20);mm_vm_frame_free(g_vm_frame_start+100*4096,1);assert(bits[100]);
 assert(!mm_vm_frame_alloc(21,&a));assert(!mm_vm_frame_alloc(0,&a));assert(!mm_vm_frame_alloc(257,&a));assert(!mm_vm_frame_alloc(1,NULL));
 /* Random contiguous requests compared against an exhaustive oracle. */
 memset(bits,0,sizeof bits);bits[100]=true;g_vm_frame_used=1;g_vm_frame_hint=0;
 srand(7);
 for(unsigned k=0;k<10000;k++){
  unsigned n=1+rand()%24;bool possible=false;unsigned run=0;
  for(unsigned i=0;i<256;i++){run=bits[i]?0:run+1;if(run>=n)possible=true;}
  bool before[256];memcpy(before,bits,sizeof bits);
  assert(mm_vm_frame_alloc(n,&a)==possible);
  if(possible){unsigned first=(a-g_vm_frame_start)/4096;for(unsigned i=first;i<first+n;i++)assert(!before[i] && bits[i]);}
  if(rand()%2){unsigned first=rand()%256,nfree=1+rand()%24;if(first+nfree<=256)mm_vm_frame_free(g_vm_frame_start+first*4096,nfree);}
  unsigned used=0;for(unsigned i=0;i<256;i++)used+=bits[i];assert(used==g_vm_frame_used && bits[100]);
 }
 munmap(pool,256*4096);puts("PASS VM allocator: linear sequential allocation, zeroing, wrap/hint-spanning runs, reserved frames, exhaustion and 10000 oracle comparisons");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-frame-') as folder:
 p=Path(folder);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
