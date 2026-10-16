"""Validate/recover the kernel's optional BFS4 metadata undo log offline."""
import os, struct, zlib
MAGIC=0x314a3442
RECORD=0x524a3442
START=3840
LIMIT=120

def crc(data):return zlib.crc32(data)&0xffffffff

def header(fd,boot_sectors=4096):
    partition=boot_sectors-4096
    if partition<0:raise ValueError('invalid BFS4 boot reservation')
    sectors=os.fstat(fd.fileno()).st_size//512
    headers=[]
    for bank in range(2):
        fd.seek((partition+START+bank)*512);raw=fd.read(512)
        if len(raw)!=512:raise ValueError('truncated BFS4 journal')
        h=struct.unpack_from('<7I',raw)
        if (h[0]==MAGIC and h[3]<=1 and h[4]<=LIMIT and
            (h[3] or not h[4]) and h[5]==boot_sectors and h[6]==sectors and
            crc(raw[:508])==struct.unpack_from('<I',raw,508)[0]):headers.append((h,raw))
    if headers:
        if len(headers)==1:return headers[0]
        delta=(headers[1][0][1]-headers[0][0][1])&0xffffffff
        return headers[1 if 0<delta<0x80000000 else 0]
    fd.seek((partition+START)*512)
    if any(fd.read((2+2*LIMIT)*512)):raise ValueError('unknown/damaged BFS4 journal; preserve image for inspection')
    return None

def require_checkpoint(fd,boot_sectors=4096):
    h=header(fd,boot_sectors)
    if h and h[0][3]:raise ValueError('BFS4 transaction pending; recover a stopped image before editing/extracting')

def recover(path,boot_sectors=4096):
    from pathlib import Path
    image=path.resolve()
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():continue
        try:
            if 'qemu-system' not in (process/'comm').read_text():continue
            if any(fd.resolve()==image for fd in (process/'fd').iterdir()):
                raise ValueError('stop QEMU before recovering its disk')
        except OSError:pass
    with path.open('r+b') as fd:
        found=header(fd,boot_sectors)
        if not found or not found[0][3]:return 0
        h,raw=found;records=[];partition=boot_sectors-4096
        for i in range(h[4]):
            fd.seek((partition+START+2+2*i)*512);r=fd.read(512);data=fd.read(512)
            if len(r)!=512 or len(data)!=512:raise ValueError('truncated journal record')
            magic,epoch,target,data_crc=struct.unpack_from('<4I',r)
            if (magic!=RECORD or epoch!=h[2] or not boot_sectors<=target<h[6] or
                crc(r[:508])!=struct.unpack_from('<I',r,508)[0] or crc(data)!=data_crc):
                raise ValueError('invalid journal record; recovery refused without writing')
            records.append((target,data))
        for target,data in reversed(records):
            fd.seek(target*512);fd.write(data)
        fd.flush();os.fsync(fd.fileno())
        empty=bytearray(512);sequence=(h[1]+1)&0xffffffff
        struct.pack_into('<7I',empty,0,MAGIC,sequence,h[2],0,0,h[5],h[6])
        struct.pack_into('<I',empty,508,crc(empty[:508]))
        fd.seek((partition+START+(sequence&1))*512);fd.write(empty)
        fd.flush();os.fsync(fd.fileno())
        return len(records)
