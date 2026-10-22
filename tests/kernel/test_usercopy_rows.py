#!/usr/bin/env python3
"""Production strided copy: exact rows, one pin, faults and bounds."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/usercopy.c').read_text();body=s[s.index('bool copy_rows_from_user('):s.index('bool copy_to_user(')]
copy_body=s[s.index('bool copy_from_user('):s.index('/* Pin a strided image once.')]
ipc=(root/'kernel/linux/i386_ipc.inc').read_text();start=ipc.index('            uint32_t offset=(sy*',ipc.index('if(request==BLES_GFX_PRESENT)'));block=ipc[start:ipc.index('            bles_gfx_preserve_occluded',start)]
wrapper='''
#define BK_EFAULT 14
typedef struct {uint32_t *pixels,*scratch;uint32_t width;} test_gfx;
static int present_copy(test_gfx *gfx,uint32_t address,uint32_t sx,uint32_t sy,uint32_t sw,uint32_t sh){
 struct {uint32_t pixels;} args={address};
'''+block+'return 0;}\n'
code=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
static unsigned spaces,pins,pin_calls,span_seen,copies,pin_limit;
static bool retired,fail_pin,deny;
static uint32_t task_current_process_id(void){return 7;}
static bool paging_process_pin(uint32_t p){assert(p==7);if(retired)return false;spaces++;return true;}
static void paging_process_unpin(uint32_t p){assert(p==7&&spaces&&!pins);spaces--;}
static bool pager_pin(uint32_t p,uint32_t a,uint32_t n,bool w){assert(p==7&&spaces&&!pins&&!w&&a&&n);pin_calls++;span_seen=n;if(fail_pin||(pin_limit&&n>pin_limit))return false;pins++;return true;}
static void pager_unpin(uint32_t p,uint32_t a,uint32_t n){assert(p==7&&spaces&&pins&&a&&n==span_seen);pins--;}
static bool user_access_ok(const void*p,uint32_t n,bool w){assert(p&&spaces&&pins&&n==span_seen&&!w);return !deny;}
static void kmemcpy(void*d,const void*s,unsigned n){assert(spaces&&pins);copies++;memcpy(d,s,n);}
''' +body+copy_body+wrapper+r'''
int main(void){
 unsigned size=3*1024*1024;
 uint8_t *map=mmap(NULL,size+4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(map!=MAP_FAILED && !mprotect(map+size,4096,PROT_NONE));
 for(unsigned i=0;i<size;i++)map[i]=(i*37U+i/7919U)&255;
 unsigned stride=760*4,rowbytes=420*4,rows=430,span=(rows-1)*stride+rowbytes;
 uint8_t *src=map+size-span,*dst=malloc(rows*rowbytes+32);assert(dst);
 memset(dst,0xa5,rows*rowbytes+32);
 assert(copy_rows_from_user(dst+16,src,rowbytes,rows,stride));
 assert(pin_calls==1 && copies==rows && span_seen==span && !spaces&&!pins);
 for(unsigned y=0;y<rows;y++)assert(!memcmp(dst+16+y*rowbytes,src+y*stride,rowbytes));
 for(unsigned i=0;i<16;i++)assert(dst[i]==0xa5 && dst[16+rows*rowbytes+i]==0xa5);
 for(unsigned mode=0;mode<3;mode++){
  fail_pin=mode==0;deny=mode==1;retired=mode==2;memset(dst,0xa5,100);
  assert(!copy_rows_from_user(dst,src,7,5,11));assert(!spaces&&!pins);
  for(unsigned i=0;i<100;i++)assert(dst[i]==0xa5);
 }
 fail_pin=deny=retired=false;pin_calls=0;
 assert(!copy_rows_from_user(dst,src,7,5,6));
 assert(!copy_rows_from_user(dst,src,0x80000000U,3,0x80000000U));
 assert(!copy_rows_from_user(dst,(void*)0xfffffff0U,16,2,16));
 assert(!copy_rows_from_user(NULL,src,7,5,11));
 assert(copy_rows_from_user(NULL,NULL,0,42,0));
 assert(copy_rows_from_user(NULL,NULL,42,0,0));assert(!pin_calls);
 // Exercise the actual rootless PRESENT copy block under bulk-pin pressure.
 test_gfx gfx={NULL,(uint32_t *)(dst+16),760};pin_limit=rowbytes;pin_calls=copies=0;
 assert(!present_copy(&gfx,(uint32_t)(uintptr_t)src,0,0,420,430));
 assert(pin_calls==rows+1 && copies==rows && !spaces&&!pins);
 for(unsigned y=0;y<rows;y++)assert(!memcmp(dst+16+y*rowbytes,src+y*stride,rowbytes));
 pin_limit=1;assert(present_copy(&gfx,(uint32_t)(uintptr_t)src,0,0,420,430)==-14 && !spaces&&!pins);
 pin_limit=0;
 free(dst);munmap(map,size+4096);
 puts("PASS production strided copy: 430 rows in one pin, exact padding/guard bounds, OOM/permission/retired-space failures, overflow and empty spans");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-usercopy-rows-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
