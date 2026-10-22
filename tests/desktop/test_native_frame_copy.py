#!/usr/bin/env python3
"""The real native consumer uses one fenced GPU blit and retains ownership."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/linux/i386_gfx3d.inc').read_text()
consumer = source[source.index('int32_t bk_vision_native_frame('):]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h"
#define BK_EACCES 13
#define BK_EFAULT 14
#define BK_EIO 5
#define KDOMAIN_GFX 8
typedef struct {int unused;} gui_window_t;
typedef struct {
 uint32_t width,height,virgl_context,native_consumers;
 uint32_t gallium_surfaces[4],gallium_readers[4],gallium_serial[4];
 uint8_t gallium_states[4];bool gallium_failed,force_present;
} bles_gfx_context;
static bles_gfx_context gfx;
static uint32_t bles_native_pending_count;
static int selected=1,linux_io_lock,domain;
static bool valid=true,copy_ok=true,store_ok=true;
static unsigned copies;
static uint32_t task_current_process_id(void){return 9;}
static uint32_t gui_vision_owner_pid(void){return 9;}
static bool paging_user_range_permitted(void *p,unsigned n,bool write){(void)n;(void)write;return p && valid;}
static void kernel_domains_enter(unsigned d){assert(d==8 && !domain);domain=1;}
static void kernel_domains_exit(unsigned d){assert(d==8 && domain);domain=0;}
static bles_gfx_context *bles_native_window(gui_window_t *w){assert(w && domain);gfx.native_consumers++;return &gfx;}
static int bles_native_latest(bles_gfx_context *g){assert(g==&gfx);return selected;}
static int bles_native_ready(bles_gfx_context *g,unsigned slot){assert(g==&gfx);return slot==0;}
static void bles_native_fail(bles_gfx_context *g){
 g->gallium_failed=true;
 for(unsigned i=0;i<4;i++)if(g->gallium_states[i]==1){g->gallium_states[i]=0;bles_native_pending_count--;}
}
static bool gfx3d_virgl(bles_virgl_t *a,void *data){
 (void)data;assert(domain && gfx.native_consumers==1);
 assert(a->abi==1 && a->op==BLES_VIRGL_PRESENT && !a->context);
 assert(a->p[0]==gfx.gallium_surfaces[selected] && a->p[1]==900 && a->p[3]==4);
 copies++;a->p[2]=77;return copy_ok;
}
static unsigned kspin_lock_irqsave(int *p){assert(p==&linux_io_lock);return 0;}
static void kspin_unlock_irqrestore(int *p,unsigned f){(void)p;(void)f;}
static bool copy_to_user(void *to,const void *from,unsigned n){if(!store_ok)return false;memcpy(to,from,n);return true;}
''' + consumer + r'''
static void reset(void){
 gfx=(bles_gfx_context){.width=640,.height=480,.virgl_context=42,.force_present=true,
 .gallium_surfaces={100,101,102,103},.gallium_states={1,1,1,0},.gallium_serial={1,2,3,0}};
 bles_native_pending_count=3;selected=1;copies=0;copy_ok=store_ok=valid=true;
}
int main(void){
 gui_window_t w;uint32_t width=0,height=0;reset();
 assert(bk_vision_native_frame(&w,0,&width,&height)==1 && width==640 && height==480);
 assert(!copies && bles_native_pending_count==3 && !gfx.native_consumers);
 assert(bk_vision_native_frame(&w,900,&width,&height)==1 && copies==1);
 assert(gfx.gallium_states[1]==2 && gfx.gallium_readers[1]==77 && !gfx.force_present);
 assert(!gfx.gallium_states[0] && gfx.gallium_states[2]==1 && bles_native_pending_count==1);
 assert(!domain && !gfx.native_consumers);
 reset();selected=3;gfx.gallium_states[1]=0;gfx.gallium_states[3]=1;gfx.gallium_serial[3]=4;
 assert(bk_vision_native_frame(&w,900,&width,&height)==1 && gfx.gallium_readers[3]==77);
 reset();copy_ok=false;
 assert(bk_vision_native_frame(&w,900,&width,&height)==-5 && gfx.gallium_failed);
 assert(!bles_native_pending_count && !gfx.native_consumers && !domain);
 reset();valid=false;
 assert(bk_vision_native_frame(&w,900,&width,&height)==-14 && !copies && !domain);
 reset();store_ok=false;
 assert(bk_vision_native_frame(&w,900,&width,&height)==-14 && copies==1);
 assert(gfx.gallium_states[1]==2 && gfx.gallium_readers[1]==77 && !gfx.native_consumers);
 puts("PASS: exact native GPU blit, real reader fence, fourth slot, coalescing, failed copy and user-memory race preserve ownership");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-native-copy-') as directory:
    p = Path(directory); (p / 'test.c').write_text(code)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
