#!/usr/bin/env python3
"""Production mode enumeration and rollback across depth/geometry failures."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2];s=(ROOT/'kernel/drivers/graphics/ati_rage128_dvr.c').read_text()
def extract(sig):
 a=s.index(sig);return s[a:s.index('\n}',a)+2]+'\n'
code='''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define kprintf(...) ((void)0)
#define GFX_MODE_ATI_RAGE128 7U
typedef void pci_device_t;
typedef struct {uint32_t mode,framebuffer;uint16_t width,height,pitch;uint8_t bpp;} gfx_info_t;
typedef struct {uint16_t width,height;uint8_t bpp;} gfx_display_mode_t;
'''+ '\n'.join(re.findall(r'^#define R128_[^\n]+',s,re.M))+'\n'
a=s.index('typedef struct {\n    uint16_t width, height;');b=s.index('static r128_state_t g_r128;',a);code+=s[a:b]+'static r128_state_t g_r128;\n'
code+='''
static uint32_t regs[2048],pll[64];
static unsigned fail_step,step;
static bool next_step(void){return ++step!=fail_step;}
static void r128_write(uint32_t reg,uint32_t v){assert(reg/4<2048);regs[reg/4]=v;}
static uint32_t r128_read(uint32_t reg){assert(reg/4<2048);return regs[reg/4];}
static uint32_t r128_pll_read(uint8_t r){return pll[r];}
static void r128_pll_write(uint8_t r,uint32_t v){pll[r]=v;}
static bool r128_pll_commit(void){return true;}
static void r128_spin_delay(uint32_t n){(void)n;}
static bool r128_wait_idle(void){return true;}
static bool r128_engine_init(void){return next_step();}
static uint32_t r128_align_up(uint32_t n,uint32_t a){return (n+a-1)&~(a-1);}
static bool r128_program_pll(uint32_t khz){pll[7]=khz;return next_step();}
static bool r128_program_dda(uint32_t khz){regs[R128_DDA_CONFIG/4]=khz;return next_step();}
static bool r128_write_mode_registers(const r128_mode_t *m,uint16_t pitch){regs[R128_CRTC_PITCH/4]=pitch;regs[R128_CRTC_H_TOTAL_DISP/4]=m->h_total;return next_step();}
static bool r128_program_panel_scaler(const r128_mode_t *m){regs[R128_FP_HORZ_STRETCH/4]=m->width;return next_step();}
static bool r128_panel_mode(uint16_t w,uint16_t h,r128_mode_t *m){(void)w;(void)h;(void)m;return false;}
'''
for sig in ['static uint32_t r128_bytes_per_pixel(', 'static uint32_t r128_pitch_units(',
 'static const r128_mode_t *r128_find_mode(', 'static bool r128_mode_allowed(',
 'static bool r128_list_modes(', 'static bool r128_restore_display(',
 'static void r128_mode_checkpoint(', 'static bool r128_set_mode(']:code+=extract(sig)
code+='''
int main(void) {
 g_r128.active=g_r128.accel_ok=g_r128.direct_modeset_ok=g_r128.display_saved=true;
 g_r128.bpp=32;g_r128.width=800;g_r128.height=600;g_r128.pitch=3200;
 g_r128.native_width=1400;g_r128.native_height=1050;g_r128.cursor_offset=8*1024*1024-1024;
 g_r128.fb_bar=0xe0000000;g_r128.fb_offset=4096;
 gfx_info_t info={7,0xe0001000,800,600,3200,32};
 gfx_display_mode_t modes[16];unsigned n;
 assert(r128_list_modes(modes,16,&n)&&n==10&&g_r128.bpp==32);
 unsigned small16=0,native16=0;
 for(unsigned i=0;i<n;i++){if(modes[i].bpp==16)small16++;if(modes[i].width==1400&&modes[i].bpp==16)native16++;}
 assert(small16==5&&native16==1);
 assert(r128_set_mode(&info,1024,768,16)&&info.pitch==2048&&info.bpp==16);
 // Every failure after programming starts must restore the preceding 16-bit
 // mode, not the original 800x600x32 BIOS mode.
 uint32_t saved_pitch=regs[R128_CRTC_PITCH/4],saved_pll=pll[7];
 for(unsigned fail=1;fail<=5;fail++) {
  step=0;fail_step=fail;
  assert(!r128_set_mode(&info,1400,1050,32));
  assert(info.width==1024&&info.height==768&&info.bpp==16&&info.pitch==2048);
  assert(g_r128.width==1024&&g_r128.height==768&&g_r128.bpp==16);
  assert(regs[R128_CRTC_PITCH/4]==saved_pitch&&pll[7]==saved_pll);
 }
 fail_step=0;step=0;assert(r128_set_mode(&info,1400,1050,16)&&info.pitch==2816);
 assert(!r128_set_mode(&info,1400,1050,8));
 puts("PASS: native 16/32 mode catalog, aligned pitch and previous-mode rollback at every programming stage");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
