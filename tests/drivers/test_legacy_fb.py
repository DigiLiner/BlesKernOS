#!/usr/bin/env python3
"""Exercise actual historical PCI matching and firmware aperture validation."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def strip(s):return re.sub(r'^#include .*\n','',s,flags=re.M)
source=(ROOT/'kernel/drivers/graphics/legacy_fb.c').read_text()
for group in ('ati','nvidia','legacy'):
 source=source.replace('#include "families/'+group+'_vbe_ids.inc"',(ROOT/('kernel/drivers/graphics/families/'+group+'_vbe_ids.inc')).read_text())
s=strip(source);s=s[:s.index('static bool activate(')]
code='''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
'''
for h in ['gfx.h','gfx_driver.h','pci.h']:code+=strip((ROOT/'kernel/include'/h).read_text())
code+=s+r'''
static pci_bar_info_t bars[6];
bool pci_get_bar_info(const pci_device_t *device,uint8_t index,pci_bar_info_t *out){(void)device;if(index>=6 || !bars[index].size)return false;*out=bars[index];return true;}
int main(void){
 pci_device_t device={.class_code=3,.subclass=0};
 for(unsigned i=0;i<sizeof(families)/sizeof(families[0]);i++){device.vendor_id=families[i].vendor;device.device_id=families[i].device;assert(identify(&device)==&families[i]);device.subclass=2;assert(!identify(&device));device.subclass=0;}
 device.vendor_id=0xffff;assert(!identify(&device));
 gfx_info_t info={.framebuffer=0xfc000000,.width=800,.height=600,.pitch=3200,.bpp=32};assert(valid_layout(&info));
 bars[0]=(pci_bar_info_t){.base=0xfc000000,.size=4*1024*1024};assert(aperture_contains(&device,&info));
 bars[0].size=1024;assert(!aperture_contains(&device,&info));bars[0].size=4*1024*1024;bars[0].is_io=true;assert(!aperture_contains(&device,&info));bars[0].is_io=false;
 bars[0].is_64=true;device.bars[1]=1;assert(!aperture_contains(&device,&info));device.bars[1]=0;assert(aperture_contains(&device,&info));
 info.framebuffer=0xffffff00;assert(!valid_layout(&info));info.framebuffer=0xfc000000;info.pitch=3199;assert(!valid_layout(&info));
 info.pitch=800;info.bpp=8;assert(valid_layout(&info));info.width=65535;assert(!valid_layout(&info));info.width=800;info.bpp=15;assert(!valid_layout(&info));
 puts("PASS: exact PCI framebuffer catalog, supported depths, short pitch/overflow, BAR capacity/type/64-bit rejection");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['gcc','-std=c11','-O1','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
