#!/usr/bin/env python3
"""Exercise production realtime interpolation, RTC resync and ABI conversion."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/api/syscall.c').read_text()
body=s[s.index('static kspinlock_t g_linux_realtime_lock'):s.index('static void linux_fill_stat64')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef int kspinlock_t;
#define KSPINLOCK_INITIALIZER 0
static unsigned calls,locked;
static uint32_t ticks,hz=300,rtc=1700000000U;
static uint32_t pit_get_ticks(void){return ticks;}
static uint32_t pit_get_frequency_hz(void){return hz;}
static uint32_t linux_realtime_seconds_uncached(void){assert(locked);calls++;return rtc;}
static uint32_t kspin_lock_irqsave(kspinlock_t *lock){(void)lock;assert(!locked);locked=1;return 0;}
static void kspin_unlock_irqrestore(kspinlock_t *lock,uint32_t irq){(void)lock;(void)irq;assert(locked);locked=0;}
''' + body + r'''
int main(void){
 ticks=299;uint64_t start=linux_realtime_nanoseconds(),last=start;
 assert(start==1700000000996666666ULL && calls==1);
 /* The RTC stays in its old second while the PIT fraction wraps. */
 ticks++;uint64_t next=linux_realtime_nanoseconds();
 assert(next==1700000001000000000ULL && next>last && calls==1);
 for(unsigned i=1;i<=30000;i++){
  ticks=299+i;rtc=1700000000U+i/300;
  uint64_t now=linux_realtime_nanoseconds();assert(now>=last);
  assert(now==1700000000000000000ULL+((uint64_t)299+i)*1000000000ULL/300);
  last=now;
 }
 assert(calls==101); /* one sample/s, not 300/s */
 for(unsigned i=0;i<100;i++)assert(linux_realtime_nanoseconds()==last);
 assert(calls==101 && linux_realtime_seconds()==last/1000000000ULL);
 /* Real external clock changes are still observed at the next resync. */
 ticks+=300;rtc+=3601;next=linux_realtime_nanoseconds();
 assert(next/1000000000ULL==rtc && calls==102);
 ticks+=300;rtc-=7200;next=linux_realtime_nanoseconds();
 assert(next/1000000000ULL==rtc && calls==103);
 /* A tick wrap must advance the fraction, not jump backwards. */
 g_linux_realtime_valid=false;ticks=UINT32_MAX-10;rtc=1700000000;
 start=linux_realtime_nanoseconds();ticks=9;next=linux_realtime_nanoseconds();
 assert(next-start>=66666666ULL && next-start<=66666667ULL);
 /* Long silence and a frequency change are handled without 32-bit overflow. */
 ticks+=0x80000000U;rtc+=(uint32_t)(0x80000000ULL/300);
 next=linux_realtime_nanoseconds();assert(next/1000000000ULL>=rtc);
 hz=100;ticks=125;rtc=12345;assert(linux_realtime_nanoseconds()==12345250000000ULL);
 hz=0;rtc=67890;assert(linux_realtime_nanoseconds()==67890000000000ULL);
 puts("PASS Linux realtime: 30000 ticks/101 RTC samples, continuous fraction, external adjustments, tick wrap, long gaps, frequency and zero-Hz fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-realtime-') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
