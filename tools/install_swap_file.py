#!/usr/bin/env python3
"""Reserve a contiguous /SYSTEM/SWAP.BKS in an offline BFS4 disk atomically."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import bfs4
from bfs4_update import Editor
from mkswap_image import build_header

ROOT=Path(__file__).resolve().parents[1]
SWAP_PATH='/SYSTEM/SWAP.BKS'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('image',type=Path)
    p.add_argument('--mib',type=int,default=32)
    p.add_argument('--update-boot',action='store_true',help='install the current compiled kernel and stage2')
    a=p.parse_args();image=a.image.resolve()
    if not 1<=a.mib<=1024:p.error('--mib must be between 1 and 1024')
    # Never edit an image that a running VM has open.
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():continue
        try:
            if 'qemu-system' not in (process/'comm').read_text():continue
            for fd in (process/'fd').iterdir():
                if fd.resolve()==image:p.error('stop QEMU before updating its disk')
        except (OSError,PermissionError):pass
    temporary=Path(str(image)+'.swap-install')
    if temporary.exists():p.error('temporary swap installation already exists')
    bfs4.check(image)
    subprocess.run(['cp','--reflink=auto','--preserve=all',str(image),str(temporary)],check=True)
    editor=None
    try:
        editor=Editor(temporary);size=a.mib*1024*1024;header=build_header(size)
        try:inode=editor.find(SWAP_PATH)
        except FileNotFoundError:inode=None
        if inode:
            v=editor.inodes[inode]
            if v[2]!=size or v[3]&0o170000!=bfs4.MODE_FILE or v[9]!=1 or v[26]:
                raise ValueError('existing SWAP.BKS has a different size/type or is fragmented; left untouched')
            start,count=editor.extents(v)[0]
            if count!=size//4096 or editor.read(start,512)!=header:
                raise ValueError('existing SWAP.BKS is not a valid swap image; left untouched')
            print('Keeping existing reserved swap:',SWAP_PATH)
        else:
            editor.update(SWAP_PATH,bytes(header)+bytes(size-512))
            inode=editor.find(SWAP_PATH);v=editor.inodes[inode]
            if v[9]!=1 or v[26]:raise ValueError('swap must have one contiguous extent')
            v[3]=bfs4.MODE_FILE|0o600
            raw=bytearray(struct.pack(bfs4.INODE_FORMAT,*v))
            struct.pack_into('<I',raw,248,bfs4.crc(raw[:248]+bytes(4)+raw[252:]))
            editor.table[inode*256:(inode+1)*256]=raw
        editor.finish();editor=None
        if a.update_boot:
            kernel=(ROOT/'build/kernel.bin').read_bytes();stage=(ROOT/'build/stage2.bin').read_bytes()
            if len(kernel)>1280*1024 or len(stage)>4096:raise ValueError('boot payload exceeds reserved space')
            with temporary.open('r+b') as f:
                f.seek(512);f.write(stage.ljust(4096,b'\0'))
                f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'));f.flush();os.fsync(f.fileno())
        bfs4.check(temporary);os.replace(temporary,image)
        print(f'{image}: {SWAP_PATH}, {a.mib} MiB reserved; no second disk needed')
    finally:
        if editor:editor.fd.close()
        temporary.unlink(missing_ok=True)

if __name__=='__main__':main()
