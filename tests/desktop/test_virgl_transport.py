#!/usr/bin/env python3
"""Exercise production raw VirGL transport with a recording host queue."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/drivers/graphics/virtio_gpu_dvr.c').read_text()
names=['ctrl_hdr','get_capset','ctx_create','resource_create_3d','box','transfer_host_3d','cmd_submit']
types=[]
for name in names:
    pattern=r'typedef struct PACKED \{[^}]*\} virtio_gpu_'+name+r'_t;'
    types.append(re.search(pattern,source).group())
constants='\n'.join(line for line in source.splitlines() if line.startswith('#define VIRTIO_GPU_CMD_') or line.startswith('#define VIRTIO_GPU_RESP_'))
head=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PACKED __attribute__((packed))
#define kmalloc malloc
#define kfree free
#define kmemset memset
#define kmemcpy memcpy
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
'''
stubs=r'''
struct mock_used {uint16_t idx;};
typedef struct {bool ready;uint32_t owner,deferred_count,used_shadow;struct mock_used *used;volatile uint16_t *notify;uint16_t index;} virtio_queue_t;
static struct mock_used mock_used;
static uint16_t mock_notify;
static uint32_t mock_ticks;
static uint32_t pit_get_ticks(void){return mock_ticks;}
static void virtio_barrier(void){}
static struct {uint32_t virgl_capset_size,virgl_capset_id,virgl_capset_version,virgl_context_id,completed_fence;bool context_init_available,virgl_commands_ready;virtio_queue_t controlq;} g_virtio={128,2,2,1,7,true,true,{true,0,0,0,&mock_used,&mock_notify,0}};
static bool virtio_queue_reap_deferred(virtio_queue_t *q){(void)q;assert(0);return false;}
static void virtio_queue_unlock(virtio_queue_t *q){q->owner=0;}
static unsigned submitted,transfers,created,destroyed,unrefs,attached,last_context,async_submits,barriers;
static unsigned native_imports,native_detaches,native_owner;
static bool host_pending[128];
static bool fail_create;
static uint32_t virtio_gpu_next_resource(void){static uint32_t id=100;return ++id;}
static bool virtio_gpu_resource_attach(uint32_t id,void *p,uint32_t size){assert(id && p && size);return true;}
static bool virtio_gpu_resource_unref(uint32_t id){assert(id);assert(!host_pending[g_virtio.virgl_context_id]);unrefs++;return true;}
static bool virtio_gpu_context_resource(uint32_t op,uint32_t id){assert(id);if(op==VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE)attached++;if(id==999){assert(g_virtio.virgl_context_id==native_owner);if(op==VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE)native_imports++;else native_detaches++;}return true;}
static bool virtio_gpu_control_command(void *request,uint32_t reqsize,void *response,uint32_t ressize,uint32_t expected,bool fenced,uint32_t *out){
    (void)response;(void)ressize;(void)expected;(void)out;
    virtio_gpu_ctrl_hdr_t *h=request;assert(reqsize>=sizeof(*h));
    last_context=h->ctx_id;
    if(h->type==VIRTIO_GPU_CMD_CTX_CREATE){assert(fenced && h->ctx_id>1);created++;}
    if(h->type==VIRTIO_GPU_CMD_CTX_DESTROY){assert(fenced && !host_pending[h->ctx_id]);destroyed++;}
    if(h->type==VIRTIO_GPU_CMD_RESOURCE_CREATE_3D && fail_create)return false;
    if(h->type==VIRTIO_GPU_CMD_SUBMIT_3D){submitted++;assert(((virtio_gpu_cmd_submit_t *)request)->size==reqsize-sizeof(virtio_gpu_cmd_submit_t));
     if(!fenced){async_submits++;host_pending[h->ctx_id]=true;}
     else {host_pending[h->ctx_id]=false;if(!((virtio_gpu_cmd_submit_t *)request)->size)barriers++;}}
    if(h->type==VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D || h->type==VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D){if(h->type==VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D)assert(fenced);host_pending[h->ctx_id]=!fenced;transfers++;}
    return true;
}
typedef struct {uint32_t resource,width,height;} virtio_gpu_3d_surface_t;
static virtio_gpu_3d_surface_t native_target={999,64,32};
static unsigned native_blits;
static bool desktop_flushed;
static bool vg_flush(void){assert(g_virtio.virgl_context_id==1);desktop_flushed=true;return true;}
static bool virtio_gpu_seal_vbo(uint32_t *fence){assert(g_virtio.virgl_context_id==1 && desktop_flushed);desktop_flushed=false;*fence=25;return true;}
static virtio_gpu_3d_surface_t *vg_surface(uint32_t h){return h==0x123?&native_target:NULL;}
#define VIRGL_CMD0(cmd,obj,len) ((cmd)|((obj)<<8)|((len)<<16))
static bool virtio_gpu_submit_fenced_async(const uint32_t *c,uint32_t n,uint32_t *fence){
 assert(n==22 && c[0]==VIRGL_CMD0(16U,0U,21U));
 assert(g_virtio.virgl_context_id==native_owner);
 assert(c[4]==999 && c[6]==1 && c[10]==64 && c[11]==32);
 assert(c[17]==0 && c[20]==32);native_blits++;*fence=30+native_blits;
 host_pending[native_owner]=true;return true;
}
#include "kernel/drivers/graphics/virtio_gpu_gallium.inc"
int main(void){
 /* Fixed-function native capture shares context 1, with no raw import. */
 native_owner=1;
 bles_virgl_t fixed={.abi=1,.op=BLES_VIRGL_PRESENT,.p={0x123,0x123,0,4}};
 assert(vg_raw_call(&fixed,NULL) && fixed.p[2]==31 && desktop_flushed);
 fixed.p[3]=2;fixed.p[0]=0;g_virtio.completed_fence=30;
 g_virtio.controlq.deferred_count=1;mock_notify=0xffff;
 assert(vg_raw_call(&fixed,NULL) && !fixed.p[0] && mock_notify==0);
 mock_notify=0xffff;
 assert(vg_raw_call(&fixed,NULL) && !fixed.p[0] && mock_notify==0xffff);
 mock_ticks++;
 assert(vg_raw_call(&fixed,NULL) && !fixed.p[0] && mock_notify==0);
 g_virtio.controlq.deferred_count=0;
 assert(vg_raw_call(&fixed,NULL) && !fixed.p[0]);g_virtio.completed_fence=31;
 assert(vg_raw_call(&fixed,NULL) && fixed.p[0]);host_pending[1]=false;
 native_blits=0;desktop_flushed=false;g_virtio.completed_fence=7;

 bles_virgl_t a={.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));unsigned c1=a.context;
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));unsigned c2=a.context;assert(c1!=c2);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_RESOURCE_CREATE,.context=c1,.p={2,1,2,16,16,1,1,0,0,0,1024}};
 assert(vg_raw_call(&a,NULL));unsigned res=a.p[0];assert(attached==1 && g_virtio.virgl_context_id==1);
 unsigned char data[1024]={0};
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_WRITE,.context=c2,.bytes=1024,.p={res,0,64,1024,0,0,0,16,16,1,0,0}};
 assert(!vg_raw_call(&a,data));assert(!transfers);a.context=c1;assert(vg_raw_call(&a,data));assert(last_context==c1);
 a.p[7]=17;assert(!vg_raw_call(&a,data));a.p[7]=16;a.p[1]=1;assert(!vg_raw_call(&a,data));a.p[1]=0;a.p[11]=1;assert(!vg_raw_call(&a,data));
 uint32_t cmd[]={1U<<16,0};a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));assert(submitted==1);cmd[0]=2U<<16;assert(!vg_raw_call(&a,cmd));assert(submitted==1);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CAPS};assert(vg_raw_call(&a,NULL));assert((a.p[4]&(BLES_VIRGL_FEATURE_ASYNC_SUBMIT|BLES_VIRGL_FEATURE_ASYNC_UPLOAD))==3);
 cmd[0]=1U<<16;a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT_ASYNC,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));assert(async_submits==1 && host_pending[c1]);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_WAIT,.context=c2};assert(vg_raw_call(&a,NULL));assert(host_pending[c1] && barriers==0);
 a.context=c1;assert(vg_raw_call(&a,NULL));assert(!host_pending[c1] && barriers==1);assert(vg_raw_call(&a,NULL));assert(barriers==1);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_SUBMIT_ASYNC,.context=c1,.bytes=sizeof(cmd)};
 assert(vg_raw_call(&a,cmd));
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_READ,.context=c1,.bytes=128,.p={res,0,64,1024,0,4,0,16,2,1,256,256}};
 assert(vg_raw_call(&a,data));assert(!host_pending[c1] && !vg_raw_context(c1)->pending);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC,.context=c1,.bytes=128,.p={res,0,64,1024,0,4,0,16,2,1,256,256}};
 memset(data,0x51,128);assert(vg_raw_call(&a,data));assert(host_pending[c1] && vg_raw_context(c1)->pending);
 memset(data,0x62,128);assert(((unsigned char *)vg_raw_resource(vg_raw_context(c1),res)->backing)[256]==0x51);
 assert(vg_raw_call(&a,data));assert(((unsigned char *)vg_raw_resource(vg_raw_context(c1),res)->backing)[256]==0x62);
 a.op=BLES_VIRGL_TRANSFER_READ;assert(vg_raw_call(&a,data));assert(!host_pending[c1] && !vg_raw_context(c1)->pending);
 a.op=BLES_VIRGL_TRANSFER_WRITE_ASYNC;assert(vg_raw_call(&a,data));assert(host_pending[c1]);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_RESOURCE_DESTROY,.context=c2,.p={res}};assert(!vg_raw_call(&a,NULL));assert(!unrefs);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_DESTROY,.context=c1};assert(vg_raw_call(&a,NULL));assert(unrefs==1 && destroyed==1);
 a.context=c2;assert(vg_raw_call(&a,NULL));assert(destroyed==2);assert(!vg_raw_context(c1) && !vg_raw_context(c2));assert(g_virtio.virgl_context_id==1);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));c1=a.context;
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_CREATE};assert(vg_raw_call(&a,NULL));c2=a.context;
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_RESOURCE_CREATE,.context=c1,.p={2,1,2,64,32,1,1,0,0,0,8192}};
 assert(vg_raw_call(&a,NULL));res=a.p[0];
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_PRESENT,.context=c2,.p={res,0x123}};
 assert(!vg_raw_call(&a,NULL));assert(!native_blits);
 a.context=c1;a.p[1]=0x124;assert(!vg_raw_call(&a,NULL));assert(!native_blits);
 a.p[1]=0x123;native_target.width=65;assert(!vg_raw_call(&a,NULL));assert(!native_blits);
 native_owner=c1;native_target.width=64;
 vg_raw_context(c1)->pending=host_pending[c1]=true;
 unsigned previous_barriers=barriers;
 assert(vg_raw_call(&a,NULL));assert(native_blits==1 && native_imports==1 && !native_detaches);
 assert(vg_raw_context(c1)->pending && host_pending[c1] && barriers==previous_barriers && a.p[2]==31);
 a.p[0]=0;a.p[3]=2;assert(vg_raw_call(&a,NULL));assert(!a.p[0]);
 vg_raw_fence_complete(c1,31);assert(vg_raw_call(&a,NULL));assert(a.p[0] && g_virtio.completed_fence==7);
 a.p[0]=0;a.p[3]=1;assert(vg_raw_call(&a,NULL));assert(a.p[2]==25);
 a.p[3]=3;assert(vg_raw_call(&a,NULL));assert(!a.p[0]);
 g_virtio.completed_fence=25;assert(vg_raw_call(&a,NULL));assert(a.p[0]);
 a.p[0]=res;a.p[2]=a.p[3]=0;
 assert(vg_raw_call(&a,NULL));assert(native_blits==2 && native_imports==1 && !native_detaches);
 assert(vg_raw_context(c1)->pending && host_pending[c1]);
 vg_raw_context(c2)->native_completed=0;
 vg_raw_fence_complete(c2,0xf0000000U);assert(vg_raw_context(c2)->native_completed==0xf0000000U);
 a=(bles_virgl_t){.abi=1,.op=BLES_VIRGL_CONTEXT_DESTROY,.context=c1};assert(vg_raw_call(&a,NULL));
 assert(native_detaches==1);
 a.context=c2;assert(vg_raw_call(&a,NULL));
 puts("PASS: context isolation, transfer bounds, packet validation, asynchronous barriers, owned GPU presentation and cleanup");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-virgl-test-') as d:
    c=Path(d)/'test.c';c.write_text(head+'\n'+constants+'\n'+'\n'.join(types)+stubs)
    exe=Path(d)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
