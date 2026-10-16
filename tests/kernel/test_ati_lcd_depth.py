#!/usr/bin/env python3
"""Production M3 timings, RGB565 conversion and FIFO depth changes; ASan/UBSan."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/graphics/ati_rage128_dvr.c').read_text()
def extract(sig):
 a=s.index(sig);return s[a:s.index('\n}',a)+2]+'\n'
code='''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define TYPES_H
#include "kernel/include/rgb565.h"
#define kprintf(...) ((void)0)
'''+ '\n'.join(re.findall(r'^#define R128_[^\n]+',s,re.M))+'\n'
a=s.index('typedef struct {\n    uint16_t width, height;');code+=s[a:s.index('\ntypedef struct {',a+1)]+'\n'
code+='''
static unsigned char rom[65536];
static unsigned char *rom_base=rom;
#undef R128_VBIOS_BASE
#define R128_VBIOS_BASE ((uintptr_t)rom_base)
static struct {
 uint32_t panel_vbios_offset,saved_dda_config,saved_dda_on_off,native_pixel_khz,saved_crtc_gen_cntl;
 uint8_t bpp;bool firmware_info_valid;
 struct {uint8_t bpp;} firmware_info;
} g_r128;
static uint32_t regs[1024];
static void r128_write(uint32_t reg,uint32_t value){assert(reg/4<1024);regs[reg/4]=value;}
static uint32_t r128_read(uint32_t reg){assert(reg/4<1024);return regs[reg/4];}
'''
for sig in ['static const r128_mode_t *r128_find_mode(', 'static uint32_t r128_bytes_per_pixel(',
 'static uint16_t r128_vbios_read16(', 'static bool r128_panel_mode(', 'static bool r128_program_dda(']:code+=extract(sig)
code+='''
static void put16(unsigned at,unsigned value){assert(at+1<sizeof(rom));rom[at]=value;rom[at+1]=value>>8;}
int main(void) {
 rom[0]=0x55;rom[1]=0xaa;g_r128.panel_vbios_offset=0x400;
 put16(0x440,0x600);put16(0x600,1400);put16(0x602,1050);put16(0x605,0x800);
 // Native 1400x1050 at 121.75 MHz, valid encoded horizontal/vertical timings.
 put16(0x800,12175);put16(0x808,1864/8-1);put16(0x80a,1400/8-1);
 put16(0x80c,1488/8-1);rom[0x80e]=18;
 put16(0x80f,1088);put16(0x811,1049);put16(0x813,1052|(4<<11));
 r128_mode_t mode={0};assert(r128_panel_mode(1400,1050,&mode));
 assert(mode.h_sync_start==1488 && mode.h_sync_end==1632 && mode.pixel_khz==121750);
 assert(!r128_panel_mode(1024,768,&mode));
 put16(0x80a,1280/8-1);assert(!r128_panel_mode(1400,1050,&mode));put16(0x80a,1400/8-1);
 put16(0x800,65535);assert(!r128_panel_mode(1400,1050,&mode));put16(0x800,12175);
 put16(0x605,65520);assert(!r128_panel_mode(1400,1050,&mode));
 g_r128.panel_vbios_offset=65500;assert(!r128_panel_mode(1400,1050,&mode));
 // Convert both alignments, odd widths, and guarded destinations exactly.
 uint32_t src[31];uint16_t dst[40];
 for(unsigned i=0;i<31;i++)src[i]=i*83791U;
 for(unsigned offset=2;offset<=3;offset++)for(unsigned count=0;count<=31;count++) {
  memset(dst,0xa5,sizeof(dst));bk_rgb565_upload(dst+offset,src,count);
  for(unsigned i=0;i<count;i++) {
   uint32_t rgb=src[i];uint16_t expected=((rgb>>19)&31)<<11|((rgb>>10)&63)<<5|((rgb>>3)&31);
   assert(dst[offset+i]==expected);
  }
  for(unsigned i=0;i<offset;i++)assert(dst[i]==0xa5a5);
  for(unsigned i=offset+count;i<40;i++)assert(dst[i]==0xa5a5);
 }
 assert(bk_rgb565_decode(0xffff)==0xffffff && bk_rgb565_decode(0)==0);
 assert(bk_rgb565_encode(0xff0000)==0xf800 && bk_rgb565_encode(0x00ff00)==0x07e0);
 // BIOS-validated memory latency is retained while clock/depth and precision change.
 g_r128.firmware_info_valid=true;g_r128.firmware_info.bpp=32;
 g_r128.native_pixel_khz=65000;g_r128.saved_dda_config=128|(8<<16)|(4<<20);
 g_r128.saved_dda_on_off=(192<<16)|3584;
 unsigned depths[]={16,32};unsigned clocks[]={25175,40000,65000,108000,121750};
 for(unsigned d=0;d<2;d++)for(unsigned c=0;c<5;c++) {
  g_r128.bpp=depths[d];assert(r128_program_dda(clocks[c]));
  uint32_t cfg=regs[R128_DDA_CONFIG/4],v=regs[R128_DDA_ON_OFF/4];
  assert((cfg&R128_DDA_XFER_MASK)>0 && (v>>16)+4<(v&65535));
 }
 g_r128.saved_dda_on_off=0;assert(!r128_program_dda(65000));
 puts("PASS: bounded BIOS timings, expanded-mode rejection, guarded RGB565 and 16/32bpp FIFO thresholds");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-I'+str(ROOT),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
