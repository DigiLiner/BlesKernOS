#!/usr/bin/env python3
"""Production printer-family parser and CDC-ECM probe/TX/RX boundary checks."""
from pathlib import Path
import re, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
def source(path):
    return re.sub(r'^#include .*\n', '', (ROOT/path).read_text(), flags=re.M)
base = '''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define kmemset memset
#define kmemcpy memcpy
#define UNUSED __attribute__((unused))
'''
printer = base + '''typedef enum { LANG_TEXT, LANG_ESCP, LANG_ESCP2, LANG_PCL5, LANG_POSTSCRIPT, LANG_ZJS, LANG_AUTO } print_language_t;
static char upper_ascii(char c){return c>='a'&&c<='z'?c-'a'+'A':c;}
static unsigned text_length(const char *s){return strlen(s);}
static bool text_equal_ci(const char *a,const char *b){while(*a&&*b)if(upper_ascii(*a++)!=upper_ascii(*b++))return false;return *a==*b;}
'''
s = (ROOT/'system/services/print_families.inc').read_text()
printer += s[:s.index('static bool profile_auto_detect(')] + r'''
int main(void) {
 print_language_t lang=LANG_AUTO;
 assert(printer_family_language("MFG:HP;MDL:LaserJet 4050;CMD:PCL,PCLXL,POSTSCRIPT;",&lang)&&lang==LANG_POSTSCRIPT);
 assert(printer_family_language("MANUFACTURER:Brother;COMMAND SET:pcl5e;",&lang)&&lang==LANG_PCL5);
 assert(printer_family_language("MFG:EPSON;CMD:ESC/P2,BDC;",&lang)&&lang==LANG_ESCP2);
 assert(printer_family_language("CMD:ESC/P;",&lang)&&lang==LANG_ESCP);
 assert(printer_family_language("MDL:HP LaserJet 1020;CMD:ZJS;",&lang)&&lang==LANG_ZJS);
 assert(!printer_family_language("MFG:HP;MDL:LaserJet;CMD:PCLXL,PCL6;",&lang));
 assert(!printer_family_language("MFG:HP;MDL:other;CMD:ZJS;",&lang));
 assert(!printer_family_language("MDL:POSTSCRIPT;CMD:NOTPOSTSCRIPT;",&lang));
 assert(!printer_family_language("CMD:PCL5evil;",&lang));
 assert(!printer_family_language("CMD:;",&lang));
 char id[300];memset(id,'X',sizeof(id));memcpy(id,"CMD:",4);id[299]=0;assert(!printer_family_language(id,&lang));
 puts("PASS: advertised printer languages, exact tokens, PCL6/unknown ZJS refusal, malformed/oversized identity");
}
'''
ecm = base + source('kernel/include/usb_core.h') + source('kernel/include/network.h') + source('kernel/include/driver.h')
ecm += '''static void kprintf(const char *fmt,...){(void)fmt;}
static int task_create(const char *n,void(*f)(void*),void *arg){(void)n;(void)f;(void)arg;return 1;}
static void task_sleep(uint32_t n){(void)n;}
static void task_exit(void){}
'''+source('kernel/drivers/network/cdc_ecm.c')+r'''
static unsigned transfers, bytes_sent, zlp, received;static uint8_t expected[6]={2,1,2,3,4,5};
static bool corrupt_mac, fail_filter;
bool netdev_present(void){return ecm.registered;}
bool netdev_register(const net_device_ops_t *ops){return ops==&net_ops;}
void netdev_unregister(const net_device_ops_t *ops){(void)ops;}
void netdev_poll(void){}
void netdev_receive(const void *frame,uint16_t n){assert(frame==ecm.rx&&n==60);received++;assert(ecm_send(frame,n));}
bool usb_class_register(const usb_class_driver_t *ops){return ops->abi_version==2;}
bool usb_class_unregister(const usb_class_driver_t *ops){(void)ops;return true;}
usb_endpoint_t *usb_interface_endpoint(usb_interface_t *i,uint8_t t,bool in,uint32_t ordinal){(void)ordinal;for(unsigned n=0;n<i->endpoint_count;n++)if((i->endpoints[n].attributes&3)==t&&USB_ENDPOINT_IN(i->endpoints[n].address)==in)return &i->endpoints[n];return NULL;}
bool usb_set_interface(usb_device_t *d,uint8_t i,uint8_t a){(void)d;return i==1&&a==1;}
bool usb_control_transfer(usb_device_t *d,uint8_t t,uint8_t r,uint16_t v,uint16_t i,void *out,uint16_t n){
 (void)d;(void)t;(void)i;
 if(r==0x43)return !fail_filter&&v==0x0e;
 if(r!=6)return false;
 uint8_t *p=out;memset(p,0,n);
 if(v==0x300){p[0]=4;p[1]=3;p[2]=9;p[3]=4;return true;}
 p[0]=26;p[1]=3;const char *mac=corrupt_mac?"010102030405":"020102030405";
 for(unsigned k=0;k<12;k++)p[2+2*k]=mac[k];return true;
}
bool usb_endpoint_transfer(usb_device_t *d,usb_endpoint_t *ep,void *data,uint32_t n,uint32_t *actual,uint32_t timeout){
 (void)d;(void)timeout;transfers++;
 if(USB_ENDPOINT_IN(ep->address)){memset(data,0x55,60);*actual=60;return true;}
 if(!n){assert(data==NULL);zlp++;}else bytes_sent+=n;
 *actual=n;return true;
}
int main(void){
 usb_device_t dev={.connected=true,.interface_count=3,.speed=USB_SPEED_FULL};
 usb_interface_t *control=&dev.interfaces[0],*idle=&dev.interfaces[1],*data=&dev.interfaces[2];
 *control=(usb_interface_t){.class_code=2,.subclass=6};
 uint8_t desc[]={5,0x24,6,0,1,13,0x24,0x0f,4,0,0,0,0,0xea,5,0,0,0};
 memcpy(control->extra,desc,sizeof(desc));control->extra_length=sizeof(desc);
 *idle=(usb_interface_t){.number=1,.class_code=0x0a};
 *data=(usb_interface_t){.number=1,.alternate=1,.class_code=0x0a,.endpoint_count=2};
 data->endpoints[0]=(usb_endpoint_t){.address=0x82,.attributes=2,.max_packet=64};
 data->endpoints[1]=(usb_endpoint_t){.address=3,.attributes=2,.max_packet=64};
 corrupt_mac=true;assert(!ecm_probe(&dev,control));corrupt_mac=false;
 fail_filter=true;assert(!ecm_probe(&dev,control));fail_filter=false;
 assert(ecm_probe(&dev,control));assert(!memcmp(ecm.mac,expected,6));assert(idle->claimed&&data->claimed);
 uint8_t frame[1537];memset(frame,0xaa,sizeof(frame));
 assert(!ecm_send(frame,13)&&!ecm_send(frame,1537)&&!ecm_send(NULL,60));
 assert(ecm_send(frame,14)&&bytes_sent==60);for(unsigned i=14;i<60;i++)assert(ecm.tx[i]==0);
 assert(ecm_send(frame,128)&&zlp==1); /* exact MPS frame terminator */
 ecm_poll();assert(received==1); /* receive callback can reply */
 ecm_disconnect(&dev,control);assert(!ecm_link()&&!ecm_send(frame,60));
 puts("PASS: CDC-ECM union/alternate/MAC, filter failure, TX padding/ZLP, RX reply, detach");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    for name, code in [('printer-families',printer), ('cdc-ecm',ecm)]:
        path=Path(tmp)/f'{name}.c';exe=Path(tmp)/name;path.write_text(code)
        subprocess.run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
