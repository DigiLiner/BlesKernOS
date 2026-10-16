#!/usr/bin/env python3
"""Graphics input readiness and unchanged device/socket poll semantics."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root/'kernel/linux/i386_ipc.inc').read_text()
body = s[s.index('static uint32_t linux_poll_raw('):s.index('static int32_t linux_poll(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define LINUX_IO_BASE 100
typedef struct {uint8_t read_pos,write_pos;} bles_gfx_context;
struct linux_inotify {unsigned used;};
typedef struct {unsigned flags,kind;bles_gfx_context *gfx;struct linux_inotify *inotify;} linux_device;
typedef struct {unsigned refs,pending,used;bool listen,read_closed,write_closed;} linux_endpoint;
typedef struct {bool connected,pipe;unsigned capacity;} linux_io_pair;
static linux_device device;
static linux_endpoint endpoints[2];
static linux_io_pair pair,*linux_pairs[]={&pair};
static linux_device *linux_device_for(int raw){return raw==50?&device:NULL;}
static linux_endpoint *linux_endpoint_for(int raw){return raw>=100 && raw<=101?&endpoints[raw-100]:NULL;}
''' + body + r'''
int main(void){
 bles_gfx_context gfx={0};device=(linux_device){.flags=2,.kind=3,.gfx=&gfx};
 assert(linux_poll_raw(50,1)==0);assert(linux_poll_raw(50,4)==4);
 gfx.write_pos=1;assert(linux_poll_raw(50,1)==1);assert(linux_poll_raw(50,5)==5);
 gfx.read_pos=1;assert(linux_poll_raw(50,1)==0);
 gfx.read_pos=63;gfx.write_pos=0;assert(linux_poll_raw(50,1)==1);
 gfx.read_pos=0;assert(linux_poll_raw(50,1)==0);
 device.gfx=NULL;assert(linux_poll_raw(50,1)==0);
 device=(linux_device){.kind=2};assert(linux_poll_raw(50,5)==1);
 device=(linux_device){.flags=1,.kind=1};assert(linux_poll_raw(50,5)==4);
 struct linux_inotify notify={0};device=(linux_device){.kind=6,.inotify=&notify};
 assert(!linux_poll_raw(50,5));notify.used=32;assert(linux_poll_raw(50,5)==1);
 assert(!linux_poll_raw(50,4));device.inotify=NULL;assert(!linux_poll_raw(50,1));
 assert(linux_poll_raw(-1,5)==32);assert(linux_poll_raw(3,5)==5);
 pair=(linux_io_pair){true,false,64};endpoints[0].refs=endpoints[1].refs=1;
 assert(linux_poll_raw(100,5)==4);endpoints[0].used=1;assert(linux_poll_raw(100,5)==5);
 endpoints[0].used=0;endpoints[1].write_closed=true;assert(linux_poll_raw(100,1)==17);
 puts("PASS graphics poll: idle/data/drained/wrapped queues, write-only requests, missing context, ordinary devices and socket EOF");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-gfx-poll-') as folder:
    path = Path(folder)
    (path/'test.c').write_text(code)
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
