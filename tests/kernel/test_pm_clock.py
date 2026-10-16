#!/usr/bin/env python3
"""Exercise production FADT parsing, torn PM reads and benchmark clock fallback."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
pm=(ROOT/'kernel/arch/x86/acpi_pm.c').read_text();perf=(ROOT/'kernel/sched/perfmon.c').read_text()
def extract(s,sig):
 a=s.index(sig);return s[a:s.index('\n}',a)+2]+'\n'
code='''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define ACPI_PM_HZ 3579545U
static uint16_t g_pm_port;
static uint32_t g_pm_mask=0xffffff;
static uint32_t samples[32],sample_count,sample_position;
static uint32_t inl(uint16_t p){assert(p==g_pm_port && sample_position<sample_count);return samples[sample_position++];}
'''
for sig in ['static uint32_t pm_u32(', 'static bool pm_checksum(', 'static bool pm_fadt(', 'bool acpi_pm_read(', 'uint32_t acpi_pm_mask(']:code+=extract(pm,sig)
code+='''
static uint32_t pit_ticks;
static bool g_tsc_supported=true;
static uint64_t tsc;
static uint32_t pit_get_ticks(void){return pit_ticks;}
static uint64_t perfmon_read_tsc(void){return tsc;}
static uint32_t perfmon_u64_to_u32(uint64_t n){return n>0xffffffff?0xffffffff:(uint32_t)n;}
static uint32_t perfmon_cycles_to_us(uint64_t n,uint32_t rate){return perfmon_u64_to_u32(n/rate);}
typedef struct {uint64_t cycles;uint32_t ticks,pm;bool pm_valid;} perfbench_stamp_t;
'''
code+=extract(perf,'static uint32_t perfbench_elapsed_us(')
code+='''
static void put32(uint8_t *p,uint32_t n){p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static void counter(uint32_t n){sample_count=3;sample_position=0;samples[0]=n;samples[1]=n;samples[2]=n+1;}
int main(void) {
 uint8_t fadt[244]={0};memcpy(fadt,"FACP",4);put32(fadt+4,116);put32(fadt+76,0x808);fadt[91]=4;
 assert(pm_fadt(fadt)&&g_pm_port==0x808&&g_pm_mask==0xffffff);
 fadt[91]=2;assert(!pm_fadt(fadt));fadt[91]=4;
 put32(fadt+76,65535);assert(!pm_fadt(fadt));put32(fadt+76,0x808);
 put32(fadt+4,244);fadt[208]=1;fadt[209]=32;put32(fadt+212,0xb008);put32(fadt+112,1<<8);
 assert(pm_fadt(fadt)&&g_pm_port==0xb008&&g_pm_mask==0xffffffff);
 fadt[208]=0;assert(!pm_fadt(fadt));fadt[208]=1;put32(fadt+216,1);assert(!pm_fadt(fadt));put32(fadt+216,0);
 assert(pm_checksum((const uint8_t*)"",0));fadt[0]++;assert(!pm_checksum(fadt,sizeof(fadt)));
 g_pm_mask=0xffffff;uint32_t value;
 samples[0]=100;samples[1]=500000;samples[2]=102;samples[3]=103;samples[4]=104;samples[5]=105;
 sample_count=6;sample_position=0;assert(acpi_pm_read(&value)&&value==104&&sample_position==6);
 samples[0]=0xfffffe;samples[1]=0;samples[2]=1;sample_count=3;sample_position=0;
 assert(acpi_pm_read(&value)&&value==0);
 for(unsigned i=0;i<24;i++)samples[i]=(i%3==1)?500000:100;
 sample_count=24;sample_position=0;assert(!acpi_pm_read(&value));
 // A wildly changing TSC must not affect PM wall time, even across two wraps.
 perfbench_stamp_t start={0,0,0xffff00,true};pit_ticks=100;tsc=2000000000ULL;
 counter((start.pm+ACPI_PM_HZ)&g_pm_mask);
 uint32_t us=perfbench_elapsed_us(start,1000,100);assert(us>=999999 && us<=1000001);
 pit_ticks=1000;counter((start.pm+(uint64_t)ACPI_PM_HZ*10)&g_pm_mask);
 us=perfbench_elapsed_us(start,1000,100);assert(us>=9999999 && us<=10000001);
 g_pm_port=0;start.pm_valid=false;pit_ticks=100;
 assert(perfbench_elapsed_us(start,1000,100)==1000000); // reject 2s TSC vs 1s PIT
 tsc=1000000000ULL;assert(perfbench_elapsed_us(start,1000,100)==1000000);
 puts("PASS: FADT ports/width, torn reads/retries/wrap, fixed PM time and unstable-TSC fallback");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
