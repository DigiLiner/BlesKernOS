#!/usr/bin/env python3
"""Exercise production Rage128 panel writes and default PCM gain on the host."""
from pathlib import Path
import re, subprocess, tempfile, sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from update_hardware_image import set_master_volume

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'

ati = (ROOT / 'kernel/drivers/graphics/ati_rage128_dvr.c').read_text()
code = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TYPES_H
#include "kernel/include/sound.h"
#define kstrlen strlen
#define kstrncmp strncmp
'''
code += '\n'.join(re.findall(r'^#define R128_[^\n]+', ati, re.M)) + '\n'
start = ati.index('typedef struct {\n    uint16_t width, height;')
code += ati[start:ati.index('\ntypedef struct {', start + 1)] + '\n'
code += '''
static struct {
 uint16_t native_width,native_height;
 bool panel_native_valid;
 uint8_t bpp;
 uint32_t fb_offset,saved_crtc_gen_cntl,saved_fp_gen_cntl,
 saved_fp_horz_stretch,saved_fp_vert_stretch,saved_lvds_gen_cntl;
} g_r128;
static uint32_t regs[2048];
static uint32_t r128_read(unsigned reg){assert(reg/4<2048);return regs[reg/4];}
static void r128_write(unsigned reg,uint32_t value){assert(reg/4<2048);regs[reg/4]=value;}
'''
for signature in ['static uint32_t r128_gmc_dst_datatype(', 'static uint32_t r128_bytes_per_pixel(', 'static bool r128_write_mode_registers(', 'static bool r128_program_panel_scaler(']:
    code += function(ati, signature)
code += (ROOT / 'kernel/drivers/audio/sound_core.c').read_text().split('\n', 1)[1]
code += function((ROOT / 'kernel/audio/system_sounds.c').read_text(), 'static uint32_t ss_parse_u32(')
code += '''
static uint8_t received_gain;
static bool pcm(const uint8_t *p,uint32_t n,uint16_t rate,uint8_t gain){
 assert(p && n==1 && rate==8000);received_gain=gain;return true;
}
int main(void){
 const r128_mode_t *native=&g_r128_modes[4], *small=&g_r128_modes[1];
 assert(native->width==1400 && native->height==1050);
 g_r128.native_width=1400;g_r128.native_height=1050;g_r128.panel_native_valid=true;
 g_r128.bpp=16;
 assert(r128_write_mode_registers(native,2816));
 assert(regs[R128_CRTC_PITCH/4]==176);
 assert((regs[R128_CRTC_GEN_CNTL/4]&R128_CRTC_PIX_WIDTH_MASK)==R128_GMC_DST_16BPP);
 g_r128.bpp=32;g_r128.fb_offset=4096;
 g_r128.saved_crtc_gen_cntl=R128_CRTC_DBL_SCAN_EN|R128_CRTC_INTERLACE_EN|R128_CRTC_CSYNC_EN;
 g_r128.saved_fp_gen_cntl=0xffffffffU;
 g_r128.saved_lvds_gen_cntl=0xffffffffU;
 g_r128.saved_fp_horz_stretch=0xffffffffU;
 g_r128.saved_fp_vert_stretch=0xffffffffU;
 assert(r128_write_mode_registers(native,5600));
 assert(r128_read(R128_CRTC_PITCH)==175);
 assert(r128_read(R128_CRTC_OFFSET)==4096);
 assert(!(r128_read(R128_CRTC_GEN_CNTL)&(R128_CRTC_DBL_SCAN_EN|R128_CRTC_INTERLACE_EN|R128_CRTC_CSYNC_EN)));
 assert(r128_program_panel_scaler(native));
 assert(((r128_read(R128_FP_HORZ_STRETCH)&R128_HORZ_PANEL_SIZE)>>16)==174);
 assert((r128_read(R128_FP_VERT_STRETCH)&R128_VERT_PANEL_SIZE)==1049);
 assert(!(r128_read(R128_FP_HORZ_STRETCH)&(R128_HORZ_STRETCH_ENABLE|R128_HORZ_STRETCH_BLEND)));
 assert(!(r128_read(R128_FP_VERT_STRETCH)&(R128_VERT_STRETCH_ENABLE|R128_VERT_STRETCH_BLEND)));
 assert(!(r128_read(R128_FP_GEN_CNTL)&(R128_FP_SEL_CRTC2|R128_FP_USE_SHADOW_VEND|R128_FP_USE_SHADOW_ROWCUR|R128_FP_HORZ_DIV2_EN|R128_FP_HOR_CRT_DIV2_DIS|R128_FP_CRT_SYNC_SEL|R128_FP_USE_SHADOW_EN)));
 assert((r128_read(R128_FP_GEN_CNTL)&7)==7); /* Panel power bits preserved. */
 assert(r128_read(R128_LVDS_GEN_CNTL)==(0xffffffffU&~R128_LVDS_SEL_CRTC2));
 assert(r128_read(R128_FP_CRTC_H_TOTAL_DISP)==r128_read(R128_CRTC_H_TOTAL_DISP));
 assert(r128_read(R128_FP_CRTC_V_TOTAL_DISP)==r128_read(R128_CRTC_V_TOTAL_DISP));
 assert(r128_write_mode_registers(small,3200));
 assert(r128_program_panel_scaler(small));
 assert(r128_read(R128_FP_HORZ_STRETCH)&R128_HORZ_STRETCH_ENABLE);
 assert(r128_read(R128_FP_VERT_STRETCH)&R128_VERT_STRETCH_ENABLE);
 g_r128.bpp=8;
 assert(!r128_write_mode_registers(native,1400)); /* Unaligned pitch rejected. */
 assert(r128_write_mode_registers(native,1408));
 assert(r128_read(R128_CRTC_PITCH)==176);
 assert((r128_read(R128_CRTC_H_SYNC_STRT_WID)&0xfff)==(uint32_t)native->h_sync_start-8U+17U);
 g_r128.panel_native_valid=false;uint32_t old=regs[R128_FP_GEN_CNTL/4];
 assert(r128_program_panel_scaler(native));assert(regs[R128_FP_GEN_CNTL/4]==old);
 assert(sound_get_master_volume()==30);
 sound_driver_ops_t ops={.play_pcm_u8=pcm};g_sound_driver=&ops;uint8_t sample=128;
 assert(sound_play_pcm_u8(&sample,1,8000,255));assert(received_gain==77);
 sound_set_master_volume(60);assert(sound_play_pcm_u8(&sample,1,8000,100));assert(received_gain==60);
 sound_set_master_volume(0);assert(sound_play_pcm_u8(&sample,1,8000,255));assert(received_gain==0);
 assert(ss_parse_u32("StartupEnabled=1\\r\\n", "MasterVolume", SOUND_DEFAULT_MASTER_VOLUME)==30);
 assert(ss_parse_u32("MasterVolume=55\\r\\n", "MasterVolume", SOUND_DEFAULT_MASTER_VOLUME)==55);
 puts("PASS: native SXGA, lower-mode scaling, CRTC1 clock/shadow flags, 8/32bpp pitch and PCM gain");
}
'''
original = b'StartupEnabled=1\r\nMasterVolume=100\r\nWindowOpen=0\r\n'
assert set_master_volume(original, 30) == original.replace(b'MasterVolume=100', b'MasterVolume=30')
assert set_master_volume(b'WindowOpen=0', 30) == b'WindowOpen=0\r\nMasterVolume=30\r\n'
with tempfile.TemporaryDirectory(prefix='bk-sxga-') as tmp:
    source=Path(tmp)/'test.c'; binary=Path(tmp)/'test'
    source.write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
