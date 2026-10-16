#!/usr/bin/env python3
"""Production string copying: pinned pages, NUL boundaries and guard pages."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/usercopy.c').read_text();s=s[s.index('bool copy_string_from_user('):]
code=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
static uint32_t lo,hi;static unsigned pins,spaces,calls;static bool retired,deny,oom;
static uint32_t task_current_process_id(void){return 7;}
static bool paging_process_pin(uint32_t pid){assert(pid==7);if(retired)return false;spaces++;return true;}
static void paging_process_unpin(uint32_t pid){assert(pid==7 && spaces && !pins);spaces--;}
static bool pager_pin(uint32_t pid,uint32_t addr,uint32_t len,bool w){assert(pid==7 && spaces && !w && len && !(addr/4096!=(addr+len-1)/4096));calls++;if(oom || addr<lo || addr>=hi || len>hi-addr)return false;pins++;return true;}
static void pager_unpin(uint32_t pid,uint32_t addr,uint32_t len){(void)addr;(void)len;assert(pid==7 && spaces && pins);pins--;}
static bool user_access_ok(const void *addr,uint32_t len,bool w){(void)addr;(void)len;assert(!w && pins && spaces);return !deny;}
''' + s + r'''
int main(void){
 char *p=mmap(NULL,3*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(p!=MAP_FAILED && (uintptr_t)p<UINT32_MAX-3*4096);lo=(uint32_t)(uintptr_t)p;hi=lo+2*4096;
 assert(!mprotect(p+8192,4096,PROT_NONE));char out[9000];
 strcpy(p+20,"payload");calls=0;assert(copy_string_from_user(out,1000,p+20) && !strcmp(out,"payload") && calls==1);
 memset(p, 'a',8192);p[8191]=0;calls=0;
 assert(copy_string_from_user(out,sizeof out,p+4090) && strlen(out)==4101 && calls==2 && !pins && !spaces);
 // NUL at the last accessible byte must not touch/pin the guard page.
 calls=0;assert(copy_string_from_user(out,100,p+8191) && !out[0] && calls==1);
 p[8191]='a';calls=0;assert(!copy_string_from_user(out,100,p+8191) && calls==2 && !pins && !spaces);
 strcpy(p,"abc");calls=0;assert(copy_string_from_user(out,4,p) && !strcmp(out,"abc") && calls==1);
 assert(!copy_string_from_user(out,3,p) && out[2]==0 && !pins && !spaces);
 assert(!copy_string_from_user(out,1,p) && !copy_string_from_user(NULL,10,p) && !copy_string_from_user(out,10,NULL));
 deny=true;assert(!copy_string_from_user(out,100,p) && !pins && !spaces);deny=false;
 oom=true;assert(!copy_string_from_user(out,100,p) && !pins && !spaces);oom=false;
 retired=true;assert(!copy_string_from_user(out,100,p) && !pins && !spaces);retired=false;
 // Equivalent random strings across page boundaries, including truncation.
 for(unsigned offset=0;offset<4096;offset+=17)for(unsigned size=2;size<600;size+=31){
  memset(p,'x',8192);unsigned length=(offset+size)%500;p[offset+length]=0;
  bool ok=copy_string_from_user(out,size,p+offset);
  assert(ok==(length<size));
  if(ok)assert(strlen(out)==length);else assert(out[size-1]==0);
  assert(!pins && !spaces);
 }
 munmap(p,3*4096);puts("PASS production string copy: one pin per page, NUL/guard boundaries, truncation, failures and random spans");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-user-string-') as temp:
 p=Path(temp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
