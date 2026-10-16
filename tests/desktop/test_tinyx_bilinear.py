#!/usr/bin/env python3
"""Check the affine TinyX fast path against the general bilinear fetch math."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'ports/tinyx/upstream/tinycorelinux-tinyx/fb/fb_bles_bilinear.h'
SOURCE = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t CARD32;
typedef int32_t xFixed;
#include "fb_bles_bilinear.h"
static uint32_t state = 12345;
static uint32_t rnd(void) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static CARD32 reference(CARD32 tl, CARD32 tr, CARD32 bl, CARD32 br, unsigned wx, unsigned wy) {
    CARD32 out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned t = ((tl >> shift) & 255) * (256-wx) + ((tr >> shift) & 255) * wx;
        unsigned b = ((bl >> shift) & 255) * (256-wx) + ((br >> shift) & 255) * wx;
        out |= ((t*(256-wy) + b*wy) >> 16) << shift;
    }
    return out;
}
int main(void) {
    CARD32 top[2048], bottom[2048], out[1026], scalar[1026];
    int skip, count;
    /* Normal 640 -> 800 scaling must accelerate the interior despite both
     * border pixels falling outside the two-neighbour sampling interval. */
    assert(fbBlesBilinearSpan(800,-6554,52429,0,639,&skip,&count));
    assert(skip==1 && count==798);
    assert(fbBlesBilinearSpan(800,639*65536+6554,-52429,0,639,&skip,&count));
    assert(skip==1 && count==798);
    assert(!fbBlesBilinearSpan(10,INT32_MAX,65536,0,639,&skip,&count));
    assert(!fbBlesBilinearSpan(10,-65536,0,0,639,&skip,&count));
    assert(fbBlesBilinearSpan(10,65536,0,0,639,&skip,&count) && skip==0 && count==10);
    for(unsigned t=0;t<5000;t++) {
        int n=1+rnd()%1000, start=(int)(rnd()%200000000)-100000000;
        int step=(int)(rnd()%131073)-65536;
        int found=fbBlesBilinearSpan(n,start,step,0,639,&skip,&count);
        int total=0;
        for(int i=0;i<n;i++) {
            int x=((int64_t)start+(int64_t)i*step)>>16;
            int valid=x>=0 && x<639;
            assert(valid==(found && i>=skip && i<skip+count));total+=valid;
        }
        assert(total==(found?count:0));
    }
    for (int trial=0; trial<5000; ++trial) {
        for (int i=0;i<2048;++i) { top[i]=rnd(); bottom[i]=rnd(); }
        int width=1+rnd()%1024, dx=7;
        int step=(int)(rnd()%65537)-32768;
        int start=(step<0 ? 1024 : 24)*65536 + (rnd()&65535);
        unsigned wy=rnd()&255;
        for(int opaque=0;opaque<2;++opaque) {
            out[0]=out[width+1]=0x12345678;
            fbBlesBilinear32(top,bottom,out+1,width,start,step,dx,wy,opaque);
            fbBlesBilinear32Scalar(top,bottom,scalar,width,start,step,dx,wy,opaque);
            int v=start;
            for(int i=0;i<width;++i) {
                int x=(v>>16)+dx;
                CARD32 mask=opaque ? 0xff000000 : 0;
                CARD32 want=reference(top[x]|mask,top[x+1]|mask,bottom[x]|mask,bottom[x+1]|mask,(v>>8)&255,wy);
                assert(out[i+1]==want && scalar[i]==want);
                v+=step;
            }
            assert(out[0]==0x12345678 && out[width+1]==0x12345678);
        }
    }
    /* Every subpixel fraction, including both rounding endpoints. */
    top[0]=0x00fe01ff; top[1]=0xff02ff00; bottom[0]=0xff7f8001; bottom[1]=0x010001fe;
    for(unsigned x=0;x<256;++x) for(unsigned y=0;y<256;++y) {
        fbBlesBilinear32(top,bottom,out,1,x<<8,0,0,y,0);
        assert(out[0]==reference(top[0],top[1],bottom[0],bottom[1],x,y));
    }
    puts("TinyX bilinear: exact pixels, alpha, positive/negative steps and output bounds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-tinyx-bilinear-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(SOURCE)
    for bits in [[], ['-m32']]:
        subprocess.run(['gcc', *bits, '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
