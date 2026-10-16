#!/usr/bin/env python3
"""Production IEEE1284 parser must clamp a device's declared length to USB data."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/usb/usbclass.c').read_text()
s=s[s.index('static bool printer_get_device_id('):s.index('/* Missing files may be supplied later;')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define kmemset memset
#define kmemcpy memcpy
#define USB_RT_IN_CLASS_INTERFACE 0xa1
#define PRINTER_REQ_GET_DEVICE_ID 0
struct interface {uint8_t number,alternate;};
typedef struct {void *device;struct interface *interface;char device_id[192];} printer_context_t;
static uint16_t declared;
static bool usb_control_transfer(void *d,unsigned type,unsigned request,unsigned value,unsigned index,void *raw,unsigned size){
 (void)d;(void)value;assert(type==0xa1 && !request && index==0x302 && size==192);
 uint8_t *p=raw;p[0]=declared>>8;p[1]=declared;memset(p+2,'X',size-2);return true;
}
'''+s+r'''
int main(void){
 struct interface interface={3,2};printer_context_t printer={.interface=&interface};
 unsigned lengths[]={0,1,2,10,191,192,193,65535};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++){
  memset(printer.device_id,0,sizeof(printer.device_id));declared=lengths[i];printer_get_device_id(&printer);
  unsigned expected=declared<2?0:declared-2;if(expected>190)expected=190;
  assert(strlen(printer.device_id)==expected);
 }
 puts("PASS: USB IEEE1284 lengths 0/1/2/short/full/oversized clamp without over-read");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['gcc','-O1','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
