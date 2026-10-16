#!/usr/bin/env python3
"""Exercise the actual TinyX GetImage handler on a guarded 32 KiB stack."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[3]
source=(root/'ports/tinyx/upstream/tinycorelinux-tinyx/dix/dispatch.c').read_text()
start=source.index('int\nDoGetImage(');end=source.index('\nint\nProcGetImage(',start)
handler=source[start:end]
shim=r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
typedef uint32_t Mask,Drawable;
typedef void *pointer;
typedef struct {unsigned errorValue,sequence;int noClientException;} ClientRec,*ClientPtr;
typedef struct DrawableRec *DrawablePtr;
typedef struct {int width,height;void (*GetImage)(DrawablePtr,int,int,int,int,int,Mask,pointer);} ScreenRec;
typedef struct DrawableRec {int type,x,y,width,height,depth,realized;ScreenRec *pScreen;} DrawableRec,*WindowPtr;
typedef struct {uint8_t type,depth;uint16_t sequenceNumber;uint32_t length,visual;char pad[20];} xGetImageReply;
#define XYPixmap 1
#define ZPixmap 2
#define BadValue 2
#define BadMatch 8
#define BadAlloc 11
#define DRAWABLE_WINDOW 0
#define None 0
#define X_Reply 1
#define IMAGE_BUFSIZE (64*1024)
#define LOG2_BYTES_PER_SCANLINE_PAD 2
#define sz_xGetImageReply 32
#define PixmapBytePad(w,d) ((void)(d),(w)*4)
#define BitmapBytePad(w) ((((w)+31)/32)*4)
#define BitsPerPixel(d) ((d)>16?32:(d))
#define Ones(n) __builtin_popcount(n)
#define wBorderWidth(w) ((void)(w),0)
#define wVisual(w) ((void)(w),1)
#define SET_DBE_SRCBUF(draw,id) ((void)(draw),(void)(id))
#define ClientOrder(c) ((void)(c),0)
#define min(a,b) ((a)<(b)?(a):(b))
static DrawableRec draw;
#define SECURITY_VERIFY_DRAWABLE(p,id,c,access) do{(void)(id);(void)(c);p=&draw;}while(0)
static unsigned chunks,written,replies,allocations,frees,max_allocation;
static int fail_allocation;
static void *allocate(size_t n){if(fail_allocation)return NULL;void *p=malloc(n);if(p){++allocations;if(n>max_allocation)max_allocation=n;}return p;}
static void release(void *p){if(p)++frees;free(p);}
#define malloc allocate
#define free release
static void get_image(DrawablePtr d,int x,int y,int w,int h,int format,Mask mask,pointer out){
 (void)d;(void)x;(void)y;(void)mask;assert(out);memset(out,0x5a,(format==ZPixmap?PixmapBytePad(w,24):BitmapBytePad(w))*h);}
static void ReformatImage(void *p,int n,int bpp,int order){(void)p;(void)n;(void)bpp;(void)order;}
static int WriteToClient(ClientPtr client,int length,const void *data){
 (void)client;assert(length>=0 && data);if(length)assert(*(const unsigned char*)data==0x5a);++chunks;written+=length;return 0;}
static void WriteReplyToClient(ClientPtr c,int n,const xGetImageReply *reply){(void)c;assert(n==32 && reply->type==X_Reply);++replies;}
'''
tests=r'''
static void *exercise(void *unused){
 (void)unused;ClientRec client={0};ScreenRec screen={30000,1000,get_image};
 draw=(DrawableRec){.type=1,.width=30000,.height=1000,.depth=24,.pScreen=&screen};
 assert(sizeof(xGetImageReply)==32);
 assert(DoGetImage(&client,ZPixmap,1,0,0,800,600,~0u,NULL)==0);
 assert(written==800*600*4 && replies==1 && chunks>1 && max_allocation>=64000);
 written=0;assert(DoGetImage(&client,ZPixmap,1,0,0,20480,1,~0u,NULL)==0);
 assert(written==81920 && max_allocation==81920); /* A scanline can exceed the chunk limit. */
 written=0;assert(DoGetImage(&client,XYPixmap,1,0,0,800,600,3,NULL)==0);
 assert(written==BitmapBytePad(800)*600*2);
 xGetImageReply *image=NULL;assert(DoGetImage(&client,ZPixmap,1,0,0,800,600,~0u,&image)==0);
 assert(image && image->depth==24 && ((unsigned char*)image)[32]==0x5a);free(image);
 unsigned before=replies;fail_allocation=1;
 assert(DoGetImage(&client,ZPixmap,1,0,0,800,600,~0u,NULL)==BadAlloc);
 assert(replies==before);fail_allocation=0;
 assert(DoGetImage(&client,ZPixmap,1,0,0,0,0,~0u,NULL)==0);
 assert(DoGetImage(&client,99,1,0,0,1,1,~0u,NULL)==BadValue);
 assert(DoGetImage(&client,ZPixmap,1,-1,0,1,1,~0u,NULL)==BadMatch);
 assert(allocations==frees);return NULL;
}
int main(void){
 pthread_attr_t attr;pthread_t thread;assert(!pthread_attr_init(&attr));
 assert(!pthread_attr_setstacksize(&attr,32768));assert(!pthread_attr_setguardsize(&attr,4096));
 assert(!pthread_create(&thread,&attr,exercise,NULL));assert(!pthread_join(thread,NULL));
 pthread_attr_destroy(&attr);puts("PASS TinyX GetImage: 32 KiB stack, 64 KiB chunks, wide rows, planes, BadAlloc and balanced allocation");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-getimage-') as tmp:
 path=Path(tmp);(path/'test.c').write_text(shim+handler+tests)
 subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-pthread',str(path/'test.c'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
