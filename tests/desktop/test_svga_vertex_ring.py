#!/usr/bin/env python3
"""Delay host DMA consumption until fence waits; wrapping must not corrupt it."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/drivers/graphics/vmware_svga3d.c').read_text()
upload = source[source.index('static bool svga3d_upload_vertices('):source.index('static bool svga3d_emit_draw(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define kmemcpy memcpy
#define SVGA3D_VERTEX_RING_SLOTS 2U
#define SVGA3D_VERTEX_SLOT_BYTES 168U
#define SVGA3D_VERTEX_BUFFER_BYTES 336U
#define SVGA3D_VERTEX_SURFACE_ID 1U
#define SVGA3D_WRITE_HOST_VRAM 1U
typedef struct {float x,y,z,rhw;uint32_t color;float u,v;} gfx3d_vertex_t;
static uint8_t memory[336];
static struct {unsigned base,n;uint8_t expected[168];} transfers[128];
static unsigned transfer_count,consumed,fences[128],fence_count,waits;
static bool fail_seal;
static void *vram_pointer(void *ctx,uint32_t offset,uint32_t n) {
 (void)ctx;assert(offset+n<=sizeof(memory));return memory+offset;
}
static struct {void *context;void *(*vram_pointer)(void *,uint32_t,uint32_t);} transport={0,vram_pointer};
static struct {
 unsigned vertex_slot,vertex_slot_offset,vertex_active_slot,vertex_base,vertex_vram_offset;
 uint32_t vertex_fence[2]; __typeof__(transport) *transport;
} g_3d={.transport=&transport};
static bool transport_submit(bool wait,uint32_t *out) {
 assert(!wait && out);if(fail_seal)return false;
 fences[++fence_count]=transfer_count;*out=fence_count;return true;
}
static bool transport_wait(uint32_t fence) {
 assert(fence && fence<=fence_count);waits++;
 while(consumed<fences[fence]) {
  assert(!memcmp(memory+transfers[consumed].base,transfers[consumed].expected,transfers[consumed].n));
  consumed++;
 }
 return true;
}
static bool svga3d_surface_dma(unsigned sid,unsigned offset,unsigned pitch,unsigned bpp,
 unsigned x,unsigned y,unsigned width,unsigned height,unsigned direction) {
 assert(sid==1 && !offset && pitch==336 && bpp==1 && !y && height==1 && direction==1);
 assert(transfer_count<128);transfers[transfer_count].base=x;transfers[transfer_count].n=width;
 memcpy(transfers[transfer_count++].expected,memory+x,width);return true;
}
''' + upload + r'''
int main(void) {
 gfx3d_vertex_t vertices[3];
 for(unsigned i=0;i<64;i++) {
  memset(vertices,i+1,sizeof(vertices));
  assert(svga3d_upload_vertices(vertices,3));
 }
 assert(fence_count==31 && waits==30); /* two batches share each slot fence */
 uint32_t last;assert(transport_submit(false,&last));assert(transport_wait(last));
 assert(consumed==64);
 unsigned offset=g_3d.vertex_slot_offset,slot=g_3d.vertex_slot;
 fail_seal=true;assert(!svga3d_upload_vertices(vertices,3));
 assert(g_3d.vertex_slot==slot && g_3d.vertex_slot_offset==offset && transfer_count==64);
 puts("PASS: delayed DMA survives 16 ring wraps; fences amortized per slot; failed seal never reuses memory");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-svga-ring-') as directory:
    c = Path(directory)/'test.c'; c.write_text(code)
    exe = Path(directory)/'test'
    subprocess.run(['gcc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
