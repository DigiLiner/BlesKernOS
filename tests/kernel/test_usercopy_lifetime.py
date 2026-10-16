#!/usr/bin/env python3
"""Production user-copy paths release space and pager references on all exits."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
body=re.sub(r'^#include[^\n]*\n','',(root/'kernel/mm/usercopy.c').read_text(),flags=re.M)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned spaces,pins;
static bool retired,fail_pin,deny,write_seen;
static uint32_t task_current_process_id(void){return 7;}
static bool paging_process_pin(uint32_t p){assert(p==7);if(retired)return false;spaces++;return true;}
static void paging_process_unpin(uint32_t p){assert(p==7 && spaces && !pins);spaces--;}
static bool pager_pin(uint32_t p,uint32_t a,uint32_t n,bool w){assert(p==7 && spaces && n);(void)a;if(fail_pin)return false;pins++;write_seen=w;return true;}
static void pager_unpin(uint32_t p,uint32_t a,uint32_t n){assert(p==7 && spaces && pins && n);(void)a;pins--;}
static bool paging_user_range_ok(const void *p,uint32_t n,bool w){assert(spaces && pins);(void)p;(void)n;assert(w==write_seen);return !deny;}
static void *kmemcpy(void *d,const void *s,size_t n){assert(spaces && pins);return memcpy(d,s,n);}
''' + body + r'''
int main(void){
 char out[16]={0},in[16]="payload";
 assert(copy_from_user(out,in,8) && !strcmp(out,in) && !spaces && !pins);
 assert(copy_to_user(out,in,8) && !spaces && !pins);
 deny=true;assert(!copy_from_user(out,in,8) && !spaces && !pins);deny=false;
 fail_pin=true;assert(!copy_to_user(out,in,8) && !spaces && !pins);fail_pin=false;
 retired=true;assert(!copy_from_user(out,in,8) && !spaces && !pins);retired=false;
 assert(copy_from_user(out,NULL,0) && !spaces && !pins);
 assert(!copy_to_user(NULL,in,8));assert(copy_string_from_user(out,sizeof(out),in));
 assert(!copy_string_from_user(out,4,in) && out[3]==0 && !spaces && !pins);
 puts("PASS production usercopy pins address-space lifetime; permission/OOM/retirement failures release all references");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-usercopy-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
