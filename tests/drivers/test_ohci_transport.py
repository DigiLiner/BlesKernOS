#!/usr/bin/env python3
"""Production OHCI TD/ED, short IN, periodic interval and fail-safe ownership."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/usb/usb_ohci.c').read_text();s=s[:s.index('static bool request(')]
s=re.sub(r'^#include .*\n','',s,flags=re.M)
h=re.sub(r'^#include .*\n','',(ROOT/'kernel/include/usb_core.h').read_text(),flags=re.M)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define kmemset memset
#define kmemcpy memcpy
static void legacy_barrier(void){__sync_synchronize();}
typedef struct {void *allocation;uint8_t *data;uint32_t size;}legacy_dma_t;
static uint32_t pit_get_ticks(void);
static uint32_t pit_get_frequency_hz(void){return 1000;}
static uint32_t task_current_pid(void){return 0;}
static bool task_current_is_user(void){return false;}
static void task_sleep(uint32_t n){(void)n;}
'''+h+s+r'''
static uint8_t arena[8192] __attribute__((aligned(4096)));
static uint32_t registers[64];static unsigned mode,ticks,periodic_slots;
static uint32_t pit_get_ticks(void){
 ohci_host_t *h=&hosts[0];ticks++;
 if(mode!=4)registers[OHCI_FRAME/4]++;
 if((registers[OHCI_CONTROL/4]&0x34U)&&(h->td->info>>28)>=14U){
  periodic_slots=0;for(unsigned i=0;i<32;i++)if(h->hcca[i])periodic_slots++;
  if(mode!=3&&mode!=4){
   h->td->info=mode==2?4U<<28:0;
   h->td->current=mode==1?bus_address(h->data)+17U:0;
   h->ed->head=bus_address(h->dummy)|2U|(mode==2?1U:0U);
  }
 }
 return ticks;
}
int main(void){
 ohci_host_t *h=&hosts[0];h->registers=registers;h->ready=true;h->hcca=(uint32_t*)arena;
 h->ed=(ohci_ed_t*)(arena+256);h->td=(ohci_td_t*)(arena+288);h->dummy=(ohci_td_t*)(arena+304);h->data=arena+4096;
 uint32_t actual;uint8_t toggle=0;
 assert(run_transfer(h,1,2,USB_SPEED_FULL,64,USB_EP_BULK,OHCI_TD_OUT,0,h->data,60,&actual,&toggle,20,0));assert(actual==60&&toggle==1);
 mode=1;assert(run_transfer(h,1,2,USB_SPEED_FULL,64,USB_EP_BULK,OHCI_TD_IN,1,h->data,128,&actual,&toggle,20,0));assert(actual==17);
 mode=2;assert(!run_transfer(h,1,2,USB_SPEED_FULL,64,USB_EP_BULK,OHCI_TD_IN,1,h->data,128,&actual,&toggle,20,0));
 mode=3;toggle=1;assert(run_transfer(h,1,1,USB_SPEED_LOW,8,USB_EP_INTERRUPT,OHCI_TD_IN,1,h->data,8,&actual,&toggle,2,10));assert(!actual&&toggle==1&&periodic_slots==4);
 assert(!run_transfer(h,1,2,USB_SPEED_LOW,64,USB_EP_BULK,OHCI_TD_OUT,0,h->data,60,&actual,&toggle,20,0));
 mode=4;assert(!run_transfer(h,1,2,USB_SPEED_FULL,64,USB_EP_BULK,OHCI_TD_IN,1,h->data,128,&actual,&toggle,2,0));assert(h->faulted&&h->td->info>>28==15);
 puts("PASS: OHCI full/short/error/NAK, periodic interval, low-speed bulk refusal, stalled-frame DMA quarantine");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';e=Path(tmp)/'test';c.write_text(code)
 subprocess.run(['gcc','-std=c11','-O1','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
