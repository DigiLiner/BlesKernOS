#!/usr/bin/env python3
"""Prove native images remain immutable across delayed GPU/desktop work."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/linux/i386_gfx3d.inc').read_text()
def function(name):
    at=source.index('static ',source.index(name)-20)
    begin=source.index('{',at);depth=1;end=begin+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[at:end]
functions='\n'.join(function(n) for n in ['bles_native_fail(', 'bles_native_ready(', 'bles_native_reserve(', 'bles_native_latest('])
ipc=(root/'kernel/linux/i386_ipc.inc').read_text()
start=ipc.index('        if(gfx->width || args.width<160U')
validation=ipc[start:ipc.index('        uint32_t bytes=',start)]
assignment=next(line for line in ipc.splitlines() if 'gfx->native_queue_limit=' in line)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"
#define BK_EIO 5
#define BK_EAGAIN 11
#define BK_EINVAL 22
typedef struct {bool gallium_failed;uint32_t gallium_next,virgl_context;
    uint32_t width;
    uint32_t native_wait_limit,native_wait_readers,native_wait_ready,native_queue_limit;
    uint32_t gallium_surfaces[BLES_VIRGL_NATIVE_SLOTS],gallium_readers[BLES_VIRGL_NATIVE_SLOTS],gallium_serial[BLES_VIRGL_NATIVE_SLOTS];uint8_t gallium_states[BLES_VIRGL_NATIVE_SLOTS];} bles_gfx_context;
static uint32_t bles_native_pending_count;
static unsigned notifications;
static void vision_ipc_notify_native_gpu(void){notifications++;}
static bool capture_ready[BLES_VIRGL_NATIVE_SLOTS],reader_ready[BLES_VIRGL_NATIVE_SLOTS],fail_query,cpu_ack;
static bool gfx3d_virgl(bles_virgl_t *q,void *data) {
    (void)data;(void)cpu_ack;
    assert((q->context==42 || !q->context) && q->op==BLES_VIRGL_PRESENT && q->p[1]>=100 && q->p[1]<100+BLES_VIRGL_NATIVE_SLOTS);
    assert(q->p[3]==2 || q->p[3]==3);
    if(fail_query)return false;
    q->p[0]=q->p[3]==2?capture_ready[q->p[1]-100]:reader_ready[q->p[1]-100];return true;
}
'''+functions+r'''
static int open_policy(uint32_t flags,uint32_t width,uint32_t height,bool opened) {
    bles_gfx_context g={.width=opened?640:0},*gfx=&g;
    bles_gfx_open_t args={width,height,flags};
'''+validation+assignment+r'''
    return gfx->native_queue_limit;
}
int main(void) {
    const int expected[]={1,1,1,-22,1,-22,1,-22,-22,-22,3,-22,-22,-22,3,-22};
    for(unsigned flags=0;flags<16;flags++)assert(open_policy(flags,640,480,false)==expected[flags]);
    assert(open_policy(16,640,480,false)==-22 && open_policy(2,640,480,true)==-22);
    assert(open_policy(2,159,480,false)==-22 && open_policy(2,640,89,false)==-22);
    assert(open_policy(2,1025,480,false)==-22 && open_policy(2,640,769,false)==-22);
    bles_gfx_context g={.virgl_context=42,.native_queue_limit=3,.gallium_surfaces={100,101,102,103},.gallium_readers={11,12,13,14},.gallium_serial={1,2,3,4}};
    assert(bles_native_reserve(&g)==0);
    g.gallium_states[0]=1;bles_native_pending_count=1;
    assert(bles_native_reserve(&g)==1); /* shader captures overlap GPU work */
    g.gallium_states[1]=1;bles_native_pending_count=2;
    assert(bles_native_reserve(&g)==2);
    g.gallium_states[2]=1;bles_native_pending_count=3;
    assert(bles_native_reserve(&g)==-BK_EAGAIN);
    g.gallium_states[1]=g.gallium_states[2]=0;g.virgl_context=0;g.native_queue_limit=1;bles_native_pending_count=1;
    assert(bles_native_reserve(&g)==-BK_EAGAIN); /* CPU TNL keeps one capture ahead */
    g.virgl_context=42;g.native_queue_limit=3;
    g.gallium_states[1]=g.gallium_states[2]=g.gallium_states[3]=2;capture_ready[0]=true;
    assert(bles_native_reserve(&g)==-BK_EAGAIN); /* even a completed unread capture survives */
    capture_ready[0]=false;
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)g.gallium_states[i]=1;
    bles_native_pending_count=BLES_VIRGL_NATIVE_SLOTS;
    assert(bles_native_reserve(&g)==-BK_EAGAIN && bles_native_latest(&g)==-1);
    cpu_ack=true;assert(bles_native_reserve(&g)==-BK_EAGAIN);
    assert(bles_native_pending_count==BLES_VIRGL_NATIVE_SLOTS);
    capture_ready[1]=true;assert(bles_native_latest(&g)==1 && bles_native_pending_count==BLES_VIRGL_NATIVE_SLOTS);
    /* A fast producer must not steal the only completed capture before the
     * desktop gets to sample it. Completion is not consumer acknowledgement. */
    for(unsigned frame=0;frame<1000;frame++) {
        assert(bles_native_reserve(&g)==-BK_EAGAIN);
        assert(bles_native_latest(&g)==1 && g.gallium_states[1]==1 && bles_native_pending_count==BLES_VIRGL_NATIVE_SLOTS);
    }
    assert(notifications==1000 && g.native_wait_ready==1000);
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++)g.gallium_states[i]=2;
    bles_native_pending_count=0; /* the consumer acknowledges/coalesces captures */
    assert(bles_native_reserve(&g)==-BK_EAGAIN);
    reader_ready[1]=true;assert(bles_native_reserve(&g)==1 && bles_native_pending_count==0);
    for(unsigned i=0;i<BLES_VIRGL_NATIVE_SLOTS;i++){g.gallium_states[i]=1;capture_ready[i]=true;}
    bles_native_pending_count=BLES_VIRGL_NATIVE_SLOTS;
    g.gallium_serial[0]=0xfffffffeU;g.gallium_serial[1]=0xffffffffU;g.gallium_serial[2]=1;g.gallium_serial[3]=0xfffffffdU;
    assert(bles_native_latest(&g)==2);
    fail_query=true;assert(bles_native_latest(&g)==-BK_EIO && g.gallium_failed && !bles_native_pending_count);
    bles_native_fail(&g);assert(!bles_native_pending_count);
    fail_query=false;g.gallium_states[0]=2;assert(bles_native_latest(&g)==-1);
    puts("PASS: ACK is not completion; completed captures survive producer pressure until consumed; reader fences, latest-ready selection, wrap and cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-native-queue-') as directory:
    c=Path(directory)/'test.c';c.write_text(code);exe=Path(directory)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined','-I'+str(root),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
