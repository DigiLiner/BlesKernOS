#!/usr/bin/env python3
"""Exercise production AC97 full-width MMIO access and PCI D0 wake policy."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];s=(root/'kernel/drivers/audio/ac97.c').read_text()
a=s.index('static uint8_t ac97_inb(');b=s.index('static inline void ac97_io_wait',a)
c=s.index('static uint8_t ac97_config8(');d=s.index('static bool ac97_init(',c)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#define inb(p) ((uint8_t)(p))
#define inl(p) ((uint32_t)(p))
#define outb(p,v) ((void)(p),(void)(v))
#define outl(p,v) ((void)(p),(void)(v))
static bool g_ac97_mmio;
typedef struct {uint8_t bus,slot,function;} pci_device_t;
static pci_device_t dev,*g_ac97_pci=&dev;
static uint32_t config[64],writes,delays;static uint16_t written;
static uint32_t pci_config_read32(uint8_t b,uint8_t s,uint8_t f,uint8_t off){
 (void)b;(void)s;(void)f;assert(!(off&3));return config[off/4];
}
static uint16_t pci_config_read16(uint8_t b,uint8_t s,uint8_t f,uint8_t off){
 return (uint16_t)(pci_config_read32(b,s,f,off&~3U)>>((off&3U)*8));
}
static void pci_config_write16(uint8_t b,uint8_t s,uint8_t f,uint8_t off,uint16_t v){
 (void)b;(void)s;(void)f;assert(off==0x44);writes++;written=v;
}
static void ac97_io_wait(void){delays++;}
'''+s[a:b]+s[c:d]+r'''
int main(void){
 void *memory=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);
 uint32_t base=(uint32_t)(uintptr_t)memory;assert(base>65535);g_ac97_mmio=true;
 ac97_outb(base+0x1b,5);ac97_outw(base+0x18,0xabcd);ac97_outl(base+0x30,0x12345678);
 assert(ac97_inb(base+0x1b)==5 && ac97_inw(base+0x18)==0xabcd && ac97_inl(base+0x30)==0x12345678);
 assert(*(uint16_t *)((uint8_t *)memory+0x18)==0xabcd);
 config[1]=0x00100000;config[0x34/4]=0x40;config[0x40/4]=1;config[0x44/4]=0x8103;
 ac97_wake_device();assert(writes==1 && written==0x100 && delays==10000);
 config[0x44/4]=0x8100;ac97_wake_device();assert(writes==1);
 config[0x40/4]=0x4005;ac97_wake_device();assert(writes==1); // capability cycle is bounded
 munmap(memory,4096);puts("PASS production AC97 MMIO preserves full addresses/widths; D3->D0 preserves PME enable without W1C acknowledgement; bounded capability walk");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-ac97-access-') as folder:
 path=Path(folder)/'test.c';path.write_text(code);binary=path.with_suffix('')
 subprocess.run(['gcc','-m32','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie',str(path),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
