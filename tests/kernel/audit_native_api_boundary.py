#!/usr/bin/env python3
"""Regression: actual native dispatcher rejects kernel-memory primitives.

Runs production wrappers and ELF dispatch with low-address i386 host objects;
this exercises the boundary, not a hardware exploitation claim.
"""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
loader=(root/'kernel/exec/elf_loader.c').read_text()
dispatch=loader[loader.index('uint64_t elf_user_api_dispatch('):loader.index('const char *elf_last_user_api_name(')]
wrapper=re.sub(r'^#include[^\n]*\n','',(root/'kernel/api/native_memory.c').read_text(),flags=re.M)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define MM_SUBSYSTEM_COUNT 8
#define BK_EFAULT 14
#define kprintf(...) ((void)0)
typedef struct {size_t total_bytes,used_bytes,free_bytes,reserved_bytes;} system_memory_info_t;
static void mm_get_system_info(system_memory_info_t *p){*p=(system_memory_info_t){100,40,60,0};}
static bool bk_sys_memory_info(system_memory_info_t *p){mm_get_system_info(p);return true;}
static uint8_t user[8192] __attribute__((aligned(4096))),source[8192] __attribute__((aligned(4096)));
static uint32_t kernel_owned=0xb1e50000;
static unsigned pins,spaces,heap_pins,raw_calls,mutations,free_calls;
static bool fail_second,deny_payload,readonly,retiring;
static void *last_free;
void *kmemcpy(void *d,const void *s,size_t n){mutations++;return memcpy(d,s,n);}
void *kmemset(void *d,int c,size_t n){mutations++;return memset(d,c,n);}
int kmemcmp(const void *a,const void *b,size_t n){return memcmp(a,b,n);}
size_t kstrlen(const char *s){return strlen(s);}
int kstrcmp(const char *a,const char *b){return strcmp(a,b);}
int kstrncmp(const char *a,const char *b,size_t n){return strncmp(a,b,n);}
char *kstrcpy(char *d,const char *s){return strcpy(d,s);}
char *kstrncpy(char *d,const char *s,size_t n){return strncpy(d,s,n);}
char *kstrcat(char *d,const char *s){return strcat(d,s);}
size_t bk_string_length(const char *s){return strlen(s);}
int bk_string_compare(const char *a,const char *b){return strcmp(a,b);}
char *bk_string_copy_n(char *d,const char *s,size_t n){return strncpy(d,s,n);}
char *bk_string_concat(char *d,const char *s){return strcat(d,s);}
int bk_memory_compare(const void *a,const void *b,size_t n){return memcmp(a,b,n);}
static uint32_t mm_user_heap_available(uint32_t a,uint32_t p){(void)p;uint32_t b=(uintptr_t)user,c=(uintptr_t)source;if(a>=b && a-b<sizeof(user))return sizeof(user)-(a-b);if(a>=c && a-c<sizeof(source))return sizeof(source)-(a-c);return 0;}
void kfree(void *p){(void)p;assert(0);}
void *krealloc(void *p,size_t n){(void)p;(void)n;assert(0);return NULL;}
void bk_sys_free(void *p){(void)p;assert(0);}
void *bk_sys_realloc(void *p,size_t n){(void)p;(void)n;assert(0);return NULL;}
bool mm_set_allocation_owner(void *p,uint32_t id){(void)p;(void)id;assert(0);return false;}
bool mm_set_allocation_subsystem(void *p,uint8_t s){(void)p;(void)s;assert(0);return false;}
static bool trusted_service;
static uint32_t gui_vision_owner_pid(void){return trusted_service?7:6;}
static bool mm_user_publish(void *p,uint32_t pid){return p==user && pid==7;}
static bool mm_user_subsystem(void *p,uint8_t s,uint32_t pid){return p==user && s<8 && pid==7;}
static uint32_t task_current_process_id(void){return 7;}
static bool inside(uint32_t a,uint32_t n,void *p,uint32_t len){uint32_t b=(uint32_t)(uintptr_t)p;return a>=b && n<=len && a-b<=len-n;}
static bool user_access_ok(void *p,uint32_t n,bool w){uint32_t a=(uintptr_t)p;return inside(a,n,user,sizeof(user)) || (!w && inside(a,n,source,sizeof(source)));}
static bool pager_pin(uint32_t p,uint32_t a,uint32_t n,bool w){assert(p==7);if(fail_second && pins)return false;if(readonly && w)return false;(void)a;(void)n;pins++;return true;}
static void pager_unpin(uint32_t p,uint32_t a,uint32_t n){assert(p==7 && pins);(void)a;(void)n;pins--;}
static bool paging_process_pin(uint32_t p){assert(p==7);if(retiring)return false;spaces++;return true;}
static void paging_process_unpin(uint32_t p){assert(p==7 && spaces);spaces--;}
static uint32_t paging_pager_entry(uint32_t p,uint32_t a){(void)p;(void)a;return 0;}
static bool mm_user_heap_pin(uint32_t a,uint32_t n,uint32_t p,void **t){assert(p==7);*t=NULL;if(deny_payload)return false;if(inside(a,n,user,sizeof(user))){*t=user;heap_pins++;}return true;}
static void mm_unpin_address_allocation(void *t){assert(t==user && heap_pins);heap_pins--;}
static bool mm_user_free(void *p,uint32_t pid){assert(pid==7);if(p && p!=user)return false;free_calls++;last_free=p;return true;}
static void *mm_user_realloc(void *p,size_t n,uint32_t pid){assert(pid==7);return p==user && n<sizeof(user)?user:NULL;}
''' + wrapper + r'''
static unsigned g_user_thunk_count=1;
static uint32_t g_user_targets[1],g_user_api_domains[1];
static char g_user_api_labels[1][32]={{"primitive"}};
static const char *g_last_user_api_name;
static uint32_t *g_user_errno_slot;
static bool elf_api_desktop_target(uint32_t p){(void)p;return false;}
static void *api_resolve_desktop(void *p){return p;}
typedef void gui_desktop_t;
static void kernel_domains_enter(uint32_t p){(void)p;}
static void kernel_domains_exit(uint32_t p){(void)p;}
static void task_user_api_guard_enter(const char *n,uint32_t t,uint32_t k){(void)n;(void)t;(void)k;}
static void task_user_api_guard_leave(void){}
uint64_t elf_api_call_raw(uint32_t t,const uint32_t *a,uint32_t *c){(void)t;(void)a;(void)c;raw_calls++;assert(0);return 0;}
''' + dispatch + r'''
static uint64_t invoke(void *fn,void *dst,const void *src,uint32_t len){
 uint32_t a[16]={(uint32_t)(uintptr_t)dst,(uint32_t)(uintptr_t)src,len};
 g_user_targets[0]=(uint32_t)(uintptr_t)fn;bool valid=false;uint32_t cleanup=99;
 uint64_t r=elf_user_api_dispatch(0,a,&valid,&cleanup);
 assert(valid && !cleanup && !raw_calls && !spaces && !pins && !heap_pins);return r;
}
int main(void){
 uint32_t value=0x10203040;memcpy(source,&value,4);
 assert(!invoke(kmemcpy,&kernel_owned,source,4));assert(kernel_owned==0xb1e50000);
 assert(!invoke(memcpy,&kernel_owned,source,4));assert(kernel_owned==0xb1e50000);
 assert(!invoke(memset,&kernel_owned,(void*)0,4));assert(kernel_owned==0xb1e50000);
 assert(!invoke(kmemcpy,user,&kernel_owned,4));assert(!*(uint32_t*)user);
 assert(!invoke(kmemcpy,(void*)0xfffffff0U,source,32));
 assert(invoke(kmemcpy,user,source,4097)==(uintptr_t)user);assert(!memcmp(user,source,4097));
 memset(user,0,32);memcpy(user,"overlapping",12);invoke(memmove,user+2,user,12);assert(!memcmp(user+2,"overlapping",12));
 invoke(memset,user,(void*)0x42,32);for(unsigned i=0;i<32;i++)assert(user[i]==0x42);
 deny_payload=true;assert(!invoke(kmemcpy,user,source,4));deny_payload=false;
 readonly=true;assert(!invoke(kmemcpy,user,source,4));readonly=false;
 fail_second=true;assert(!invoke(kmemcpy,user,source,4));fail_second=false;
 retiring=true;assert(!invoke(kmemcpy,user,source,4));retiring=false;
 invoke(kfree,&kernel_owned,NULL,0);assert(!free_calls);invoke(free,user,NULL,0);assert(free_calls==1 && last_free==user);
 invoke(bk_sys_free,&kernel_owned,NULL,0);assert(free_calls==1);
 assert(!invoke(krealloc,&kernel_owned,(void*)128,0));assert(invoke(realloc,user,(void*)128,0)==(uintptr_t)user);
 invoke(mm_set_allocation_owner,user,(void*)99,0);assert(errno==EACCES);
 invoke(mm_get_system_info,&kernel_owned,NULL,0);assert(kernel_owned==0xb1e50000);
 assert(invoke(bk_sys_memory_info,user,NULL,0)==1 && ((system_memory_info_t*)user)->free_bytes==60);
 memcpy(source,"hello",6);assert(!invoke(kstrcpy,&kernel_owned,source,0));
 assert(kernel_owned==0xb1e50000);assert(!invoke(strcpy,user,&kernel_owned,0));
 assert(invoke(strcpy,user,source,0)==(uintptr_t)user && !strcmp((char*)user,"hello"));
 assert(invoke(strlen,user,NULL,0)==5);
 assert(invoke(strcmp,user,source,0)==0);
 assert(invoke(strncpy,user,source,3)==(uintptr_t)user && !memcmp(user,"hel",3));
 memset(user,0,16);memcpy(user,"pre",4);assert(invoke(strcat,user,source,0)==(uintptr_t)user && !strcmp((char*)user,"prehello"));
 assert(!invoke(kstrncpy,&kernel_owned,source,4) && kernel_owned==0xb1e50000);
 memset(source,'x',sizeof(source));assert(!invoke(strcpy,user,source,0));
 trusted_service=true;
 assert(invoke(mm_set_allocation_owner,user,NULL,0)==1);
 assert(!invoke(mm_set_allocation_owner,&kernel_owned,NULL,0));
 assert(!invoke(mm_set_allocation_owner,user,(void*)99,0));
 trusted_service=false;
 bool valid=true;uint32_t a[16]={0},c=0;elf_user_api_dispatch(1,a,&valid,&c);assert(!valid && !raw_calls);
 puts("PASS production native API blocks kernel read/write, bad ranges, readonly, payload rejection, pin failures, foreign frees, metadata mutation and invalid tokens");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-api-audit-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-fno-builtin','-fno-pie','-no-pie','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
