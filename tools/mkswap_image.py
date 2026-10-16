#!/usr/bin/env python3
"""Create a new dedicated swap image; never overwrite an existing file."""
import argparse, struct, zlib
from pathlib import Path
def build_header(size):
    if size<8192 or size%4096 or size>1024*1024*1024:
        raise ValueError('swap size must be page aligned, between 8 KiB and 1 GiB')
    header=bytearray(512)
    header[:8]=b'BKSWAP01'
    struct.pack_into('<5I',header,8,1,4096,(size-4096)//4096,8,size//512)
    struct.pack_into('<I',header,508,zlib.crc32(header[:508]))
    return header

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=Path)
    p.add_argument('--mib',type=int,default=64)
    a=p.parse_args()
    if not 1<=a.mib<=1024:p.error('--mib must be between 1 and 1024')
    size=a.mib*1024*1024
    with a.output.open('xb') as f:
        f.write(build_header(size))
        f.truncate(size)
    print(f'{a.output}: {a.mib} MiB, {(size-4096)//4096} slots')

if __name__=='__main__':main()
