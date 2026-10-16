#!/usr/bin/env python3
"""Validate BKGP and decode every PNG with the actual OS PNG/inflate code."""
import io
import struct
import subprocess
import tempfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
C_SOURCE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t *pixels; uint16_t width, height; } gui_image_t;
extern unsigned char gui_png_decode(gui_image_t *, const uint8_t *, uint32_t);
extern void gui_image_free(gui_image_t *);
void *kmalloc(unsigned int n) { return malloc(n); }
void kfree(void *p) { free(p); }
void *kmemcpy(void *d, const void *s, unsigned int n) { return memcpy(d,s,n); }
void *kmemset(void *d, int c, unsigned int n) { return memset(d,c,n); }
int kmemcmp(const void *a, const void *b, unsigned int n) { return memcmp(a,b,n); }
static uint32_t rd32(const uint8_t *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
int main(int argc, char **argv) {
    FILE *f = argc == 2 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 1;
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    if (size < 12) return 2;
    uint8_t *pak=malloc(size);
    if (!pak || fread(pak,1,size,f)!=(size_t)size) return 3;
    fclose(f);
    uint32_t count=rd32(pak+8);
    if (12ull+64ull*count > (unsigned long long)size) return 4;
    for(uint32_t i=0;i<count;i++) {
        uint8_t *entry=pak+12+64*i;
        uint32_t offset=rd32(entry+56), bytes=rd32(entry+60);
        if ((unsigned long long)offset+bytes > (unsigned long long)size) return 5;
        gui_image_t image={0};
        if(!gui_png_decode(&image,pak+offset,bytes)) { fprintf(stderr,"decode failed: %.48s\n",entry); return 6; }
        uint32_t hash=2166136261u;
        for(uint32_t pixel=0;pixel<(uint32_t)image.width*image.height;pixel++) {
            uint32_t value=image.pixels[pixel];
            for(int shift=0;shift<32;shift+=8) hash=(hash^((value>>shift)&255))*16777619u;
        }
        printf("%u %u %u %u\n",i,image.width,image.height,hash);
        gui_image_free(&image);
    }
    free(pak);return 0;
}
'''

def main():
    path = ROOT / 'assets/graphics/GRAPHICS.PAK'
    pak = path.read_bytes()
    magic, version, count = struct.unpack_from('<4sII', pak)
    assert magic == b'BKGP' and version == 1 and 0 < count <= 4096
    entries = []
    for i in range(count):
        raw, w, h, kind, depth, offset, size = struct.unpack_from('<48sHHHHII', pak, 12+i*64)
        name = raw.split(b'\0', 1)[0].decode('ascii')
        assert name and kind == 1 and depth == 32
        assert offset >= 12+64*count and offset % 4 == 0 and offset+size <= len(pak)
        entries.append((name, w, h, pak[offset:offset+size]))
    assert len({e[0].lower() for e in entries}) == count
    with tempfile.TemporaryDirectory(prefix='bk-kde-check-') as temp:
        temp = Path(temp)
        harness = temp / 'decode.c'; harness.write_text(C_SOURCE)
        binary = temp / 'decode'
        subprocess.run(['gcc', '-O2', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
                        '-Wno-pointer-to-int-cast', str(harness), str(ROOT/'gui/image.c'),
                        str(ROOT/'kernel/libc/inflate.c'), '-Wl,--gc-sections', '-o', str(binary)], check=True)
        output = subprocess.check_output([str(binary), str(path)], text=True).splitlines()
    assert len(output) == count
    for line, (name, w, h, data) in zip(output, entries):
        i, decoded_w, decoded_h, decoded_hash = map(int, line.split())
        image = Image.open(io.BytesIO(data)).convert('RGBA')
        assert image.size == (w, h) == (decoded_w, decoded_h), name
        hash_value = 2166136261
        for r,g,b,a in image.getdata():
            for channel in (b,g,r,a):
                hash_value = ((hash_value ^ channel) * 16777619) & 0xffffffff
        assert hash_value == decoded_hash, name
    print(f'PASS: {count} unique resources, bounds and all PNG pixels match the OS decoder')

if __name__ == '__main__':
    main()
