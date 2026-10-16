#!/usr/bin/env python3
"""Exercise production M3 ROM parsing and UHCI bulk staging with guarded buffers."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
def extract(path, signature):
 s=(ROOT/path).read_text();a=s.index(signature);return s[a:s.index('\n}',a)+2]
ati='kernel/drivers/graphics/ati_rage128_dvr.c'
usb='kernel/drivers/usb/usb_uhci.c'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define kprintf(...) ((void)0)
static uint8_t rom[65536];
static uint8_t *host_rom=rom;
#define R128_VBIOS_BASE ((uintptr_t)host_rom)
#define R128_VBIOS_SCAN_SIZE 65536U
#define R128_VBIOS_PANEL_X_OFFSET 25U
#define R128_VBIOS_PANEL_Y_OFFSET 27U
#define R128_MAX_COORD 2048U
#define R128_PPLL_FB3_DIV_MASK 0x7FFU
static struct {uint16_t width,height;uint32_t saved_ppll_ref_div,saved_ppll_active_div,panel_vbios_offset; bool direct_modeset_ok;uint16_t native_width,native_height;uint32_t fb_offset,cursor_offset;} g_r128;
static const void *r128_find_mode(uint16_t w,uint16_t h) {
 return ((w==640 && h==480)||(w==800 && h==600)||(w==1024 && h==768)||(w==1280 && h==1024)) ? rom : NULL;
}
'''
for f in ['static uint32_t r128_post_divisor(', 'static uint16_t r128_vbios_read16(', 'static uint32_t r128_vbios_pixel_clock(', 'static bool r128_detect_panel_vbios(']:code+=extract(ati,f)+'\n'
code+=r'''
typedef struct {uint16_t width,height;} r128_mode_t;
static uint32_t r128_align_up(uint32_t n,uint32_t a){return (n+a-1)&~(a-1);}
static uint32_t r128_bytes_per_pixel(void){return 4;}
'''
code+=extract(ati,'static bool r128_mode_allowed(')+'\n'
text=(ROOT/usb).read_text()
for name in ['UHCI_MAX_TD','UHCI_BULK_DMA_SIZE']:
 code+=re.search(r'^#define '+name+r'\s+[^\n]+',text,re.M)[0]+'\n'
code+=r'''
#define MSC_CBW_WIRE_SIZE 31U
#define kmemcpy memcpy
#define kmemset memset
typedef struct {uint8_t *bulk_buffer;} uhci_controller_t;
typedef struct {uhci_controller_t *hc;uint16_t bulk_in_packet,bulk_out_packet;} uhci_mass_device_t;
static unsigned calls,position,stop_call,fail_call,largest;
static uint8_t pattern(unsigned i){return (uint8_t)(i*37U+i/251U);}
static void uhci_dma_fence(void){}
static void usb_msc_trace_cbw_dma(uhci_mass_device_t *d,void *p){(void)d;(void)p;}
static bool uhci_bulk_batch(uhci_mass_device_t *d,bool in,void *p,uint32_t len,
 uint32_t *packets,uint32_t *bytes,bool *short_packet,const char *stage,uint32_t timeout) {
 (void)stage;(void)timeout;calls++;if(calls==fail_call)return false;
 unsigned packet=in?d->bulk_in_packet:d->bulk_out_packet;
 assert((len+packet-1)/packet<=UHCI_MAX_TD);
 if(len>largest)largest=len;
 *bytes=calls==stop_call?len/2:len;*packets=(*bytes+packet-1)/packet;
 *short_packet=calls==stop_call;
 for(unsigned i=0;i<*bytes;i++) {
  if(in)((uint8_t*)p)[i]=pattern(position+i);
  else assert(((uint8_t*)p)[i]==pattern(position+i));
 }
 position+=*bytes;return true;
}
'''
code+=r'''
typedef struct {volatile uint32_t status;uint32_t token;uintptr_t next;} uhci_td_t;
'''
expanded=text.replace('\\\n',' ')
for name in ['UHCI_TD_STS_BITSTUFF','UHCI_TD_STS_CRC_TIMEOUT','UHCI_TD_STS_BABBLE','UHCI_TD_STS_DATABUFFER','UHCI_TD_STS_STALLED','UHCI_TD_STS_ACTIVE','UHCI_TD_STS_SPD','UHCI_TD_STS_FATAL','UHCI_PID_IN']:
 code+=re.search(r'^#define '+name+r'\s+[^\n]+',expanded,re.M)[0]+'\n'
for f in ['static uint32_t uhci_td_expected_length(uint32_t token) {', 'static uint32_t uhci_td_actual_length(', 'static bool uhci_td_is_short_in(', 'static bool uhci_chain_pending(']:code+=extract(usb,f)+'\n'
code+=extract(usb,'static bool uhci_bulk_unlocked(')
code+=r'''
static void put16(unsigned at,unsigned value){rom[at]=value;rom[at+1]=value>>8;}
static void panel(unsigned signature,unsigned slot,unsigned table,unsigned w,unsigned h) {
 memset(rom,0,sizeof(rom));rom[0]=0x55;rom[1]=0xaa;
 memcpy(rom+signature,"M3      ",8);if(signature-2+20+slot*2+1<sizeof(rom))put16(signature-2+20+slot*2,table);
 if(table+28<sizeof(rom)){put16(table+25,w);put16(table+27,h);}
}
int main(void) {
 uint16_t w=777,h=555;g_r128.width=800;g_r128.height=600;
 panel(0x100,0,0x400,1024,768);
 assert(r128_detect_panel_vbios(&w,&h)&&w==1024&&h==768);
 g_r128.native_width=w;g_r128.native_height=h;g_r128.direct_modeset_ok=true;g_r128.cursor_offset=8*1024*1024;
 r128_mode_t allowed={1024,768},oversized={1280,1024};
 assert(r128_mode_allowed(&allowed)&&!r128_mode_allowed(&oversized));
 g_r128.cursor_offset=1024;assert(!r128_mode_allowed(&allowed));
 g_r128.cursor_offset=8*1024*1024;g_r128.direct_modeset_ok=false;assert(!r128_mode_allowed(&allowed));
 panel(0x100,31,0x400,1280,1024);
 assert(r128_detect_panel_vbios(&w,&h)&&w==1280&&h==1024);
 panel(0x100,0,65520,1024,768);w=777;h=555;
 assert(!r128_detect_panel_vbios(&w,&h)&&w==777&&h==555);
 panel(0x100,0,0x400,640,480);assert(!r128_detect_panel_vbios(&w,&h));
 panel(0x100,0,0x400,1366,768);assert(!r128_detect_panel_vbios(&w,&h));
 panel(65528,0,0,0,0);assert(!r128_detect_panel_vbios(&w,&h));
 memset(rom,0,sizeof(rom));assert(!r128_detect_panel_vbios(&w,&h));
 rom[0]=0x55;rom[1]=0xaa;put16(0x48,0x100);put16(0x130,0x200);put16(0x20e,2950);
 g_r128.saved_ppll_ref_div=59;g_r128.saved_ppll_active_div=260|(1U<<16);
 assert(r128_vbios_pixel_clock()==65000);
 put16(0x130,65530);assert(!r128_vbios_pixel_clock());
 put16(0x130,0x200);g_r128.saved_ppll_ref_div=0;assert(!r128_vbios_pixel_clock());
 unsigned char arena[UHCI_BULK_DMA_SIZE+32],data[124*1024+32];
 uhci_controller_t hc={arena+16};uhci_mass_device_t dev={&hc,64,64};
 const unsigned lengths[]={0,1,31,64,1024,8191,8192,8193,124*1024};
 for(unsigned packet=8;packet<=64;packet*=2) {
  dev.bulk_in_packet=dev.bulk_out_packet=packet;
  for(unsigned in=0;in<=1;in++)for(unsigned t=0;t<sizeof(lengths)/sizeof(*lengths);t++) {
   unsigned len=lengths[t],actual=999;calls=position=largest=stop_call=fail_call=0;
   memset(arena,0xa5,sizeof(arena));memset(data,0xcc,sizeof(data));
   if(!in)for(unsigned i=0;i<len;i++)data[16+i]=pattern(i);
   assert(uhci_bulk_unlocked(&dev,in,data+16,len,"test",1000,&actual));
   assert(actual==len&&calls==(len+packet*UHCI_MAX_TD-1)/(packet*UHCI_MAX_TD));
   for(unsigned i=0;i<len;i++)assert(data[16+i]==pattern(i));
   for(unsigned i=0;i<16;i++)assert(arena[i]==0xa5&&arena[sizeof(arena)-1-i]==0xa5&&data[i]==0xcc&&data[16+len+i]==0xcc);
  }
 }
 dev.bulk_in_packet=dev.bulk_out_packet=64;
 calls=position=largest=fail_call=0;stop_call=2;unsigned actual;
 assert(uhci_bulk_unlocked(&dev,true,data,20000,"short",1000,&actual)&&actual==12288&&calls==2);
 calls=position=largest=stop_call=0;fail_call=2;
 assert(!uhci_bulk_unlocked(&dev,true,data,20000,"failed",1000,&actual)&&actual==8192);
 assert(!uhci_bulk_unlocked(&dev,true,NULL,1,"null",1000,&actual));
 uhci_td_t chain[128];memset(chain,0,sizeof(chain));
 for(unsigned i=0;i<128;i++) {
  chain[i].status=UHCI_TD_STS_ACTIVE;
  chain[i].token=UHCI_PID_IN|(63U<<21);
  chain[i].next=i==127?0:(uintptr_t)&chain[i+1];
 }
 uhci_td_t *pending=chain;
 for(unsigned i=0;i<128;i++) {
  assert(uhci_chain_pending(&pending,true)&&pending==&chain[i]);
  chain[i].status=63;
 }
 assert(!uhci_chain_pending(&pending,true)&&!pending);
 pending=chain;chain[8].status=UHCI_TD_STS_STALLED;
 assert(!uhci_chain_pending(&pending,true)&&pending==&chain[8]);
 chain[8].status=UHCI_TD_STS_SPD|31;
 assert(!uhci_chain_pending(&pending,true)&&pending==&chain[8]);
 assert(!uhci_chain_pending(&pending,false)&&!pending);
 puts("PASS: guarded M3/PLL ROM parsing, USB reads/writes, short packets, failures, TD limits");
}
'''
with tempfile.TemporaryDirectory() as temp:
 c=Path(temp)/'test.c';exe=Path(temp)/'test';c.write_text(code)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
