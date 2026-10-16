#!/usr/bin/env python3
"""Exercise production ATI PLL selection and bounded MMIO waits with fake hardware."""
from pathlib import Path
import re, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/drivers/graphics/ati_rage128_dvr.c').read_text()
def extract(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]+'\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
'''
for name in ['R128_CLOCK_CNTL_INDEX','R128_CLOCK_CNTL_DATA','R128_PLL_WR_EN',
             'R128_PPLL_CNTL','R128_PPLL_REF_DIV','R128_PPLL_DIV_3',
             'R128_VCLK_ECP_CNTL','R128_HTOTAL_CNTL','R128_PPLL_RESET',
             'R128_PPLL_ATOMIC_UPDATE_EN','R128_PPLL_VGA_ATOMIC_UPDATE_EN',
             'R128_PPLL_FB3_DIV_MASK','R128_PPLL_POST3_DIV_MASK',
             'R128_VCLK_SRC_SEL_MASK','R128_VCLK_SRC_SEL_CPUCLK',
             'R128_VCLK_SRC_SEL_PPLLCLK','R128_PLL_DIV_SEL','R128_TIMEOUT']:
    code += re.search(r'^#define '+name+r'\s+[^\n]+',source,re.M)[0]+'\n'
code += r'''
static struct {uint32_t saved_ppll_active_div,native_pixel_khz,pll_min_khz,pll_max_khz;} g_r128;
static uint32_t clock_index,pll[64],poll_reads,ready_at,ticks,hz=100,advance_clock=1;
static bool commit_ok=true;
static uint32_t r128_read(uint32_t reg) {
 if(reg==R128_CLOCK_CNTL_INDEX)return clock_index;
 if(reg==R128_CLOCK_CNTL_DATA)return pll[clock_index&63];
 poll_reads++;return poll_reads>=ready_at?0xff:0x80000000U;
}
static void r128_write8(uint32_t reg,uint8_t value) {
 assert(reg==R128_CLOCK_CNTL_INDEX);clock_index=(clock_index&~255U)|value;
}
static void r128_write(uint32_t reg,uint32_t value) {
 if(reg==R128_CLOCK_CNTL_INDEX)clock_index=value;
 else {assert(reg==R128_CLOCK_CNTL_DATA);assert(clock_index&R128_PLL_WR_EN);pll[clock_index&63]=value;}
}
static bool r128_pll_commit(void){return commit_ok;}
static void r128_spin_delay(uint32_t loops){(void)loops;}
typedef unsigned perf_scope_id_t;
static uint64_t perfmon_scope_begin(void){return 0;}
static void perfmon_scope_end(perf_scope_id_t s,uint64_t t){(void)s;(void)t;}
static bool timed_out;
static uint32_t recorded_polls;
static void perfmon_scope_note(perf_scope_id_t s,uint32_t b,uint32_t p,bool failed){(void)s;(void)b;recorded_polls=p;timed_out=failed;}
static uint32_t pit_get_ticks(void){uint32_t now=ticks;ticks+=advance_clock;return now;}
static uint32_t pit_get_frequency_hz(void){return hz;}
'''
for name in ['static uint32_t r128_pll_read(', 'static void r128_pll_write(',
             'static void r128_pll_masked_write(', 'static uint32_t r128_post_divisor(',
             'static uint32_t r128_post_bits(', 'static bool r128_program_pll(',
             'static bool r128_perf_poll(']:
    code += extract(name)
code += r'''
int main(void) {
 g_r128.saved_ppll_active_div=260|(1U<<16);g_r128.native_pixel_khz=65000;
 // Firmware may scan from any of the four dividers; DIV3 can hold junk.
 for(unsigned selected=0;selected<4;selected++) {
  clock_index=selected<<8;pll[R128_PPLL_DIV_3]=0xA5F00000U;
  assert(r128_program_pll(121750));
  assert((clock_index&R128_PLL_DIV_SEL)==R128_PLL_DIV_SEL);
  unsigned feedback=pll[R128_PPLL_DIV_3]&R128_PPLL_FB3_DIV_MASK;
  unsigned post=r128_post_divisor(pll[R128_PPLL_DIV_3]);
  assert(post*121750U>=125000U && post*121750U<=250000U);
  unsigned actual=(65000U*feedback*2U+260U*post/2U)/(260U*post);
  assert(actual>121000&&actual<122500);
  assert((pll[R128_PPLL_DIV_3]&~(R128_PPLL_FB3_DIV_MASK|R128_PPLL_POST3_DIV_MASK))==0xA5F00000U);
  assert((pll[R128_VCLK_ECP_CNTL]&R128_VCLK_SRC_SEL_MASK)==R128_VCLK_SRC_SEL_PPLLCLK);
  assert(r128_program_pll(25175));
  feedback=pll[R128_PPLL_DIV_3]&R128_PPLL_FB3_DIV_MASK;
  post=r128_post_divisor(pll[R128_PPLL_DIV_3]);
  actual=(65000U*feedback*2U)/(260U*post);
  assert(actual>24900&&actual<25400);
 }
 commit_ok=false;assert(!r128_program_pll(65000));
 g_r128.saved_ppll_active_div=0;assert(!r128_program_pll(65000));
 ready_at=1;poll_reads=ticks=0;
 assert(r128_perf_poll(0x1000,255,64,0x80000000U,0)&&recorded_polls==1&&!timed_out);
 ready_at=5000;poll_reads=ticks=0;
 assert(r128_perf_poll(0x1000,255,64,0x80000000U,0)&&recorded_polls==5000);
 ready_at=~0U;poll_reads=0;ticks=~0U-3U;
 assert(!r128_perf_poll(0x1000,255,64,0x80000000U,0)&&timed_out&&recorded_polls<R128_TIMEOUT);
 advance_clock=0;poll_reads=ticks=0;
 assert(!r128_perf_poll(0x1000,255,64,0x80000000U,0)&&recorded_polls==R128_TIMEOUT);
 advance_clock=1;hz=1;poll_reads=ticks=0;
 assert(!r128_perf_poll(0x1000,255,64,0x80000000U,0)&&recorded_polls==4096);
 puts("PASS: active PLL clock, persistent DIV3, commit failure, ready/delayed/stuck MMIO and tick wrap");
}
'''
with tempfile.TemporaryDirectory() as temp:
    c=Path(temp)/'test.c';exe=Path(temp)/'test';c.write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
