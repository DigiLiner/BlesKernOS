#!/usr/bin/env python3
"""Check the exact OS inflater against stored, fixed and dynamic gzip streams."""
from pathlib import Path
import gzip
import random
import subprocess
import tempfile
import zlib
ROOT=Path(__file__).resolve().parents[2]
source='\n'.join(line for line in (ROOT/'kernel/libc/inflate.c').read_text().splitlines() if not line.startswith('#include'))
harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
'''
checks=r'''
static unsigned char *readfile(const char *path,size_t *n){FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*n=ftell(f);rewind(f);unsigned char *p=malloc(*n+1);assert(p);assert(fread(p,1,*n,f)==*n);fclose(f);return p;}
int main(int argc,char **argv){
 for(int i=1;i+1<argc;i+=2){size_t n,expected;unsigned char *input=readfile(argv[i],&n),*want=readfile(argv[i+1],&expected),*out=malloc(expected+32);assert(out);memset(out,0xa5,expected+32);
 int32_t got=inflate_gzip(input,n,out,expected?expected:1);if(got!=(int32_t)expected)fprintf(stderr,"case=%s got=%d expected=%zu\n",argv[i],got,expected);assert(got==(int32_t)expected);assert(!memcmp(out,want,expected));for(size_t j=expected;j<expected+32;j++)assert(out[j]==0xa5);
 if(expected>4){memset(out,0xa5,expected+32);assert(inflate_gzip(input,n,out,expected/2)<0);for(size_t j=expected/2;j<expected+32;j++)assert(out[j]==0xa5);}
 free(input);free(want);free(out);
 }puts("PASS: actual OS gzip inflater, stored/fixed/dynamic streams, Google response when available, output limits and guards");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-browser-gzip-') as directory:
 p=Path(directory);(p/'test.c').write_text(harness+source+checks)
 subprocess.run(['gcc','-std=c11','-O2','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 rng=random.Random(478)
 bodies=[b'',b'<html>'+('Español ñ € — fuente y texto. '*6000).encode()+b'</html>',bytes(rng.randrange(256) for _ in range(90000)),b'x'*200000]
 cases=[]
 for data in bodies:
  for strategy in [zlib.Z_DEFAULT_STRATEGY,zlib.Z_FIXED]:
   for level in [0,1,6,9]:
    compressor=zlib.compressobj(level,zlib.DEFLATED,31,8,strategy);cases.append((compressor.compress(data)+compressor.flush(),data))
 google=Path('/tmp/blesk-google-gzip.bin')
 if google.exists() and google.read_bytes()[:2]==b'\x1f\x8b':cases.append((google.read_bytes(),gzip.decompress(google.read_bytes())))
 args=[]
 for i,(compressed,data) in enumerate(cases):
  a=p/f'{i}.gz';b=p/f'{i}.raw';a.write_bytes(compressed);b.write_bytes(data);args.extend([str(a),str(b)])
 subprocess.run([str(p/'test'),*args],check=True)
