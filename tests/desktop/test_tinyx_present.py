#!/usr/bin/env python3
"""Exact framebuffer snapshots: direct writes, retry, resize, OOM and reset."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = root/'ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/bles_present.h'
code = r'''
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
static int fail;
static void *test_realloc(void *p,size_t bytes){return fail?NULL:realloc(p,bytes);}
#define realloc test_realloc
#include "bles_present.h"
#undef realloc
int main(void){
 uint8_t *base=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(base!=MAP_FAILED && !mprotect(base+4096,4096,PROT_NONE));
 uint8_t *pixels=base+4096-257;
 for(unsigned i=0;i<257;i++)pixels[i]=(uint8_t)i;
 assert(BlesFrameChanged(pixels,257));
 assert(BlesFrameChanged(pixels,257)); /* Failed publish: still pending. */
 BlesFramePublished(BlesFramePrepare(pixels,257),257);assert(!BlesFrameChanged(pixels,257));
 for(unsigned i=0;i<257;i++){
  pixels[i]^=0x80;assert(BlesFrameChanged(pixels,257));
  BlesFramePublished(BlesFramePrepare(pixels,257),257);assert(!BlesFrameChanged(pixels,257));
 }
 assert(BlesFrameChanged(pixels,129));BlesFramePublished(BlesFramePrepare(pixels,129),129);
 assert(!BlesFrameChanged(pixels,129));
 fail=1;assert(BlesFrameChanged(pixels,257));BlesFramePublished(BlesFramePrepare(pixels,257),257);
 assert(BlesFrameChanged(pixels,257)); /* Conservative until storage exists. */
 fail=0;assert(BlesFrameChanged(pixels,257));BlesFramePublished(BlesFramePrepare(pixels,257),257);
 assert(!BlesFrameChanged(pixels,257));
 const void *upload=BlesFramePrepare(pixels,257);
 uint8_t saved=((const uint8_t*)upload)[3];pixels[3]^=1;
 assert(((const uint8_t*)upload)[3]==saved);
 BlesFramePublished(upload,257);assert(BlesFrameChanged(pixels,257));
 BlesFramePublished(BlesFramePrepare(pixels,257),257);assert(!BlesFrameChanged(pixels,257));
 BlesFrameFini();assert(!blesFramePixels && !blesFrameSize && !blesFrameValid);
 assert(BlesFrameChanged(pixels,257));BlesFramePublished(BlesFramePrepare(pixels,257),257);
 assert(!BlesFrameChanged(pixels,257));assert(!BlesFrameChanged(NULL,0));
 BlesFrameFini();BlesFrameFini();munmap(base,8192);
 puts("PASS TinyX publication: direct pixel writes at every byte, unchanged suppression, failed publish retry, exact bounds, resize/OOM and repeated reset");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-tinyx-present-') as folder:
    path = Path(folder)
    (path/'test.c').write_text(code)
    subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(header.parent), str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
