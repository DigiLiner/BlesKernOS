#!/usr/bin/env python3
"""Historical GUI imports resolve the user desktop handle at the actual ELF bridge."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/exec/elf_loader.c').read_text()
body=s[s.index('extern gui_desktop_t *api_resolve_desktop'):s.index('const char *elf_last_user_api_name')]
names=list(dict.fromkeys(re.findall(r'&([a-z_]+)',body.split('uint64_t elf_user_api_dispatch',1)[0])))
a=(root/'kernel/api/api.c').read_text();resolver=a[a.index('gui_desktop_t *api_resolve_desktop'):a.index('static const char *api_export_string')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef struct {int marker;} gui_desktop_t;
static gui_desktop_t real={1},proxy={2},preview={3};
static gui_desktop_t *g_user_desktop_proxy=&proxy;
static gui_desktop_t *gui_get_desktop(void){return &real;}
static unsigned held,calls;
static gui_desktop_t *expected;
static uint32_t g_user_thunk_count=2,g_user_targets[2],g_user_api_domains[2]={7,7};
static char g_user_api_labels[2][32]={"desktop test","other"};
static const char *g_last_user_api_name;
static uint32_t errno_copy,*g_user_errno_slot=&errno_copy;
static int errno=42;
static bool native_memory_dispatch(uint32_t t,const uint32_t *a,uint64_t *r){(void)t;(void)a;(void)r;return false;}
#define kmemcpy memcpy
static void kernel_domains_enter(uint32_t d){assert(d==7 && !held);held=1;}
static void kernel_domains_exit(uint32_t d){assert(d==7 && held);held=0;}
static void task_user_api_guard_enter(const char *n,uint32_t t,uint32_t token){assert(n && t && token<2 && held);}
static void task_user_api_guard_leave(void){assert(held);}
uint64_t elf_api_call_raw(uint32_t target,const uint32_t *args,uint32_t *cleanup){assert(target && held && args[0]==(uint32_t)expected && args[15]==99);calls++;*cleanup=8;return 0x123456789ULL;}
'''+''.join(f'static int {n}(void){{return {i+1};}}\n' for i,n in enumerate(names))+resolver+body+r'''
int main(void){uint32_t args[16]={0};args[0]=(uint32_t)&proxy;args[15]=99;bool valid;uint32_t cleanup=0;
 uint32_t targets[]={'''+','.join(f'(uint32_t)&{n}' for n in names)+r'''};
 for(unsigned i=0;i<sizeof(targets)/sizeof(targets[0]);i++){
  g_user_targets[0]=targets[i];expected=&real;
  assert(elf_user_api_dispatch(0,args,&valid,&cleanup)==0x123456789ULL && valid && cleanup==8 && !held && errno_copy==42);
  assert(args[0]==(uint32_t)&proxy && args[15]==99);
 }
 args[0]=(uint32_t)&preview;expected=&preview;assert(elf_user_api_dispatch(0,args,&valid,&cleanup) && valid && !held);
 args[0]=0;expected=&real;assert(elf_user_api_dispatch(0,args,&valid,&cleanup) && valid && !held);
 g_user_targets[1]=1;args[0]=(uint32_t)&proxy;expected=&proxy;assert(elf_user_api_dispatch(1,args,&valid,&cleanup) && valid && !held);
 unsigned before=calls;assert(!elf_user_api_dispatch(2,args,&valid,&cleanup) && !valid && calls==before);
 assert(!elf_user_api_dispatch(0,NULL,&valid,&cleanup) && !valid && calls==before);
 puts("PASS legacy desktop imports, real/proxy/custom/NULL handles, unchanged caller arguments, held domains, return/cleanup/errno and invalid-token rejection");}
'''
with tempfile.TemporaryDirectory(prefix='bk-desktop-api-') as folder:
 d=Path(folder);(d/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
