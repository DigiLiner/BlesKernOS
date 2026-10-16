#!/usr/bin/env python3
"""Execute production EHCI completion loop against staged overlay/writeback."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/storage/usb_storage.c').read_text()
macros='\n'.join(line for line in s.splitlines() if line.startswith('#define ') and not line.endswith('\\'))
a=s.index('typedef struct {\n    volatile uint32_t link;');b=s.index('typedef struct {\n    uint32_t signature;',a)
types=s[a:b]
a=s.index('static bool ehci_run_qh(');b=s.index('static ehci_qh_t *ehci_alloc_qh',a)
loop=s[a:b]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PACKED __attribute__((packed))
'''+macros+'\n'+types+r'''
typedef struct {ehci_qh_t *async_head;bool faulted;} ehci_controller_t;
static uint32_t g_bot_last_td_token,g_bot_last_qh_token,g_bot_last_usb_sts,g_bot_phase;
static ehci_qh_t qh,async;static ehci_td_t td;static unsigned reads,freed;static bool unsafe,failed;
static void kprintf(const char *fmt,...){(void)fmt;}
static uint32_t pit_get_ticks(void){return 0;}
static uint32_t pit_get_frequency_hz(void){return 100;}
static uint32_t mmio_read(ehci_controller_t *hc,uint32_t off){
 (void)hc;(void)off;reads++;
 if(reads==1){qh.next_link=1;qh.token=0;} /* prefetch before active overlay */
 else if(reads==2){qh.token=TD_ACTIVE;}
 else if(reads>=4){td.token=failed?TD_HALTED:0;qh.token=td.token;}
 return 0;
}
static void mmio_write(ehci_controller_t *hc,uint32_t off,uint32_t value){(void)hc;(void)off;(void)value;}
static bool ehci_unlink_synchronized(ehci_controller_t *hc){(void)hc;assert(reads>=4);return !unsafe;}
static void ehci_free_chain(ehci_td_t *head){assert(head==&td);head->used=0;freed++;}
'''+loop+r'''
static void reset(void){memset(&qh,0,sizeof(qh));memset(&td,0,sizeof(td));qh.used=td.used=1;td.token=TD_ACTIVE;reads=freed=0;}
int main(void){
 ehci_controller_t hc={.async_head=&async};
 reset();assert(ehci_run_qh(&hc,&qh,&td,false,NULL,100));assert(reads>=4&&freed==1&&!qh.used);
 unsafe=true;reset();assert(!ehci_run_qh(&hc,&qh,&td,false,NULL,100));assert(!freed&&td.used&&qh.used);
 unsafe=false;failed=true;reset();assert(!ehci_run_qh(&hc,&qh,&td,false,NULL,100));assert(freed==1);
 puts("PASS: EHCI waits for final qTD writeback, rejects hardware errors, retains unsafe DMA");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';e=Path(tmp)/'test';c.write_text(code)
 subprocess.run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
