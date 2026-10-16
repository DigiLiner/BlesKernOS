#!/usr/bin/env python3
"""Exercise boot readiness and failures against the actual kernel wait code."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/core/kernel.c').read_text()
body = source[source.index('static bool kernel_prepare_wine(void)'):source.index('void kernel_main(void)')]
policy = (root/'kernel/include/wine_policy.h').read_text()
policy = policy[policy.index('static inline bool'):policy.index('\n#endif')]
loader = (root/'kernel/exec/elf_loader.c').read_text()
gate = loader[loader.index('    /* Apply the same RAM rule'):]
gate = gate[:gate.index('    kmemset(linux_modules')]
boot = source[source.index('void kernel_main(void)'):]
assert boot.index('kernel_prepare_wine()') < boot.index('kprintf("[BKBOOT] DESKTOP RELEASED')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static uint32_t physical_ram=256U*1024U*1024U;
static uint32_t mm_total_bytes(void){return physical_ram;}
''' + policy + r'''
static const char *g_elf_error;
static int launch_gate(const char *path){
''' + gate + r'''
 return 0;
}
static uint32_t ticks, origin;
static int scenario, registration, adopted, started, ready_calls, fail_calls, exits, sleeps;
static const char *failure;
static void bootsplash_show(const char *s, unsigned p){assert(!strcmp(s,"LOADING WINE") && p==99);}
static void bootsplash_pulse(void){}
static int elf_spawn_linux_suspended_ex(const char *p,const char *a){assert(!strcmp(p,"/WINEGATE/bin/wine-launch.ELF") && !strcmp(a,"--warmup"));return scenario==1?-1:42;}
static const char *elf_last_error(void) __attribute__((unused));
static const char *elf_last_error(void){return "mock failure";}
static void syscall_wine_boot_begin(uint32_t p){assert(p==42 && !started);registration=1;}
static void admin_adopt(unsigned id,uint32_t p){assert(id==3 && p==42 && registration && !started);adopted=1;}
static bool task_start_suspended(uint32_t p){assert(p==42 && registration && adopted);started=1;return scenario!=2;}
static bool task_request_exit_process(uint32_t p,int status){assert(p==42 && status==1);exits++;return true;}
static void admin_fail(unsigned id,const char *why){assert(id==3);failure=why;fail_calls++;}
static void admin_ready(unsigned id){assert(id==3 && started);ready_calls++;}
#define kprintf(...) ((void)0)
static uint32_t pit_get_frequency_hz(void){return scenario==7?0:100;}
static uint32_t pit_get_ticks(void){return ticks;}
static bool task_query_process(uint32_t p,bool *alive,int32_t *status,void *unused){(void)unused;assert(p==42);*alive=scenario!=3 && !(scenario==6 && sleeps>0);*status=*alive?0:9;return true;}
static int32_t syscall_wine_boot_result(void){if(scenario==4)return -1;if(scenario==5)return 0;return sleeps>0?1:0;}
static void task_sleep(unsigned t){assert(t==1);ticks++;sleeps++;}
''' + body + r'''
int main(void){
 for(scenario=0;scenario<8;scenario++){
  ticks=origin=scenario==7?UINT32_MAX:0;
  registration=adopted=started=ready_calls=fail_calls=exits=sleeps=0;failure=NULL;
  bool ok=kernel_prepare_wine();
  if(scenario==0 || scenario==7){assert(ok && ready_calls==1 && !fail_calls && !exits && sleeps==1);}
  else {assert(!ok && !ready_calls && fail_calls==1);
   const char *expected=scenario==1?"WINE_BOOT_LOAD_FAILED":scenario==2?"WINE_BOOT_START_FAILED":scenario==3||scenario==6?"WINE_BOOT_EXITED":scenario==4?"WINE_BOOT_FAILED":"WINE_BOOT_TIMEOUT";
   assert(!strcmp(failure,expected));
   assert(exits==(scenario==2||scenario==4||scenario==5));
   if(scenario==5)assert(ticks-origin==18000);
  }
 }
 const uint32_t sizes[]={16U<<20,(128U<<20)-1,128U<<20,(128U<<20)+1,129U<<20,256U<<20};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
  physical_ram=sizes[i];scenario=0;ticks=0;
  registration=adopted=started=ready_calls=fail_calls=exits=sleeps=0;
  assert(kernel_prepare_wine());
  bool enabled=physical_ram>(128U<<20);
  assert(started==enabled && ready_calls==enabled && !fail_calls);
  assert(launch_gate("/WINEGATE/bin/wine-launch.ELF")== (enabled?0:-1));
  assert(launch_gate("/winegate/BIN/WINE-LAUNCH")== (enabled?0:-1));
  assert(launch_gate("/WINEGATE/bin/wine-tool.ELF")== (enabled?0:-1));
  assert(launch_gate("/OPT/WINE/bin/wine")== (enabled?0:-1));
  assert(launch_gate("/OPT/WINE/bin/wineserver")== (enabled?0:-1));
  assert(launch_gate("/SYSTEM/SERVICES/VISION.BEX")==0);
 }
 puts("PASS physical RAM policy: 16 MiB, below/exactly/above 128 MiB, 129 and 256 MiB; no low-RAM bootstrap, case-insensitive launcher/tool/runtime gate, native apps allowed");
 puts("PASS Wine boot: register before runnable, real readiness, missing loader, start failure, exit, failed prime, timeout, exit/readiness race, timer wrap and zero-frequency fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-wine-boot-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(code)
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
