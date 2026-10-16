#!/usr/bin/env python3
"""Run HP firmware and production USB printer transport with fault injection."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/usb/usbclass.c').read_text()
firmware=s[s.index('static bool printer_prepare_firmware_inner('):s.index('static bool printer_attach(')]
write=s[s.index('static int32_t printer_provider_write('):s.index('static bool usbclass_probe(')]
header=re.sub(r'^#include .*\n','',(ROOT/'kernel/drivers/usb/hp_firmware.h').read_text(),flags=re.M)
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define kfree free
#define kprintf(...) ((void)0)
#define USB_RT_OUT_CLASS_INTERFACE 0x21
#define PRINTER_REQ_SOFT_RESET 2
struct endpoint{uint8_t toggle;};
struct device{bool connected;uint16_t vendor_id,product_id;};
struct interface{uint8_t number;};
typedef struct {bool active;struct device *device;struct endpoint *bulk_out,*bulk_in;struct interface *interface;bool firmware_pending,firmware_failed;volatile uint32_t io_lock;uint8_t status;char device_id[192];} printer_context_t;
static printer_context_t printer;static struct device device;static struct endpoint endpoint;static struct interface interface;
static uint8_t container[9000];static bool exists,valid,ready;static int fault;static unsigned calls,bytes,resets;
static uint32_t pit_get_frequency_hz(void){return 100;}
static void task_sleep(uint32_t ticks){assert(ticks==10);}
static bool printer_get_device_id(printer_context_t *p){if(ready && bytes>=sizeof(container))strcpy(p->device_id,"MFG:HP;FWVER:2026;");return true;}
static bool printer_get_status(printer_context_t *p){p->status=0x18;return true;}
static bool vfs_read_all(const char *path,void **data,uint32_t *size){assert(!strcmp(path,"/SYSTEM/FIRMWARE/SIHP1020.DL"));if(!exists)return false;*size=sizeof(container);*data=malloc(*size);memcpy(*data,container,*size);if(!valid)((uint8_t *)*data)[0]=0;return true;}
static bool usb_endpoint_transfer(struct device *d,struct endpoint *e,void *data,uint32_t size,uint32_t *actual,uint32_t timeout){(void)e;(void)data;assert(size<=4096 && timeout>0);calls++;*actual=size;if(fault==1){*actual=size-1;return true;}if(fault==2)return false;if(fault==3){d->connected=false;return false;}bytes+=size;return true;}
static bool usb_clear_endpoint_halt(struct device *d,struct endpoint *e){(void)d;(void)e;return true;}
static bool usb_control_transfer(struct device *d,unsigned a,unsigned b,unsigned c,unsigned f,void *p,unsigned n){(void)d;(void)a;(void)b;(void)c;(void)f;(void)p;(void)n;resets++;return true;}
static printer_context_t *printer_provider_at(uint32_t index){return index==0 && printer.active && device.connected?&printer:NULL;}
'''+header+firmware+write+r'''
static void seed(void){
 memset(&printer,0,sizeof(printer));memset(container,0,sizeof(container));
 memcpy(container,"\xbe\xef" "ABCDEFGH",10);uint32_t length=sizeof(container)-17;for(unsigned i=0;i<4;i++)container[10+i]=length>>(24-i*8);container[17]=0xc0;container[18]=0xde;
 device=(struct device){true,0x03f0,0x2b17};printer.active=true;printer.device=&device;printer.bulk_out=&endpoint;printer.interface=&interface;printer.firmware_pending=true;
 calls=bytes=resets=0;exists=valid=ready=true;fault=0;
}
int main(void){
 seed();assert(!hp_firmware_path(0x1234,0x2b17));assert(hp_firmware_path(0x03f0,0x4117));assert(!hp_firmware_ready_id("F"));assert(!hp_firmware_ready_id("FWVER"));assert(hp_firmware_ready_id("MFG:HP;FWVER:1;"));
 assert(hp_firmware_valid(container,sizeof(container)));assert(!hp_firmware_valid(container,10));assert(!hp_firmware_valid(container,0xffffffff));container[10]=1;assert(!hp_firmware_valid(container,sizeof(container)));
 seed();exists=false;assert(!printer_prepare_firmware(&printer) && !printer.firmware_failed && calls==0 && !printer.io_lock);
 seed();valid=false;assert(!printer_prepare_firmware(&printer) && printer.firmware_failed && calls==0);
 seed();assert(printer_prepare_firmware(&printer) && !printer.firmware_pending && calls==3 && bytes==sizeof(container));assert(printer_prepare_firmware(&printer) && calls==3);
 seed();strcpy(printer.device_id,"FWVER:1;");exists=false;assert(printer_prepare_firmware(&printer) && calls==0);
 for(int f=1;f<=3;f++){seed();fault=f;assert(!printer_prepare_firmware(&printer) && printer.firmware_failed && calls==1);assert(!printer_prepare_firmware(&printer) && calls==1 && !printer.io_lock);}
 seed();ready=false;assert(!printer_prepare_firmware(&printer) && printer.firmware_failed && calls==3);
 seed();printer.io_lock=1;assert(!printer_prepare_firmware(&printer) && calls==0);printer.io_lock=0;
 seed();printer.firmware_pending=false;assert(printer_provider_write(0,container,sizeof(container),5000)==sizeof(container));assert(calls==3 && !printer.io_lock);
 seed();printer.firmware_pending=false;fault=1;assert(printer_provider_write(0,container,sizeof(container),5000)==-5);assert(calls==1 && resets==1 && !printer.io_lock);
 puts("PASS: HP IDs/container/readiness, missing/wrong firmware, short/timeout/disconnect, serialized chunked printing without replay");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['gcc','-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
