#!/usr/bin/env python3
"""Build the normal VMware disk with a genuinely larger BFS4 filesystem."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import bfs4
from bfs4_update import Editor


def paths(editor):
    result={1:'/'}
    def name(number):
        if number not in result:
            inode=editor.inodes[number]
            result[number]=name(inode[1]).rstrip('/')+'/'+inode[27].split(b'\0')[0].decode()
        return result[number]
    return {name(i):v for i,v in editor.inodes.items()}


def build(source, mbr, output, size_mib=4096, qemu_img='qemu-img'):
    if size_mib < 16:raise ValueError('VMware disk must be at least 16 MiB')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vmware-',dir=output.parent) as work:
        work=Path(work);tree=work/'root';partition=work/'partition.img';disk=work/'disk.img'
        # Rebuild allocation geometry: enlarging only the VMDK leaves BFS4 small.
        bfs4.extract(source,tree)
        with source.open('rb') as stream:boot=stream.read(bfs4.BOOT_SECTORS*512)
        # A rebuilt filesystem has a different geometry and no pending journal.
        boot=bytearray(boot);boot[3840*512:4096*512]=bytes(256*512)
        with partition.open('wb') as stream:
            stream.truncate((size_mib-1)*1024*1024)
            stream.write(boot)
        bfs4.build(partition,tree,size_mib-1,'BLESKERNOS')
        old=Editor(source);new=Editor(partition)
        try:
            original=paths(old)
            for guest,inode in paths(new).items():
                prior=original[guest]
                assert inode[2]==prior[2],guest
                # Keep type (including links), modes, owner and timestamps.
                inode[3:9]=prior[3:9]
                raw=bytearray(struct.pack(bfs4.INODE_FORMAT,*inode))
                struct.pack_into('<I',raw,248,0)
                struct.pack_into('<I',raw,248,bfs4.crc(raw))
                number=inode[0];new.table[number*256:(number+1)*256]=raw
            new.finish()
        finally:old.fd.close();new.fd.close()
        bfs4.check(partition)
        subprocess.run(['python3',str(Path(__file__).with_name('build_usb_image.py')),str(disk),str(partition),str(mbr)],check=True)
        assert disk.stat().st_size==size_mib*1024*1024
        temporary=work/'disk.vmdk'
        subprocess.run([qemu_img,'convert','-f','raw','-O','vmdk','-o','subformat=monolithicSparse,compat6=on,adapter_type=ide',str(disk),str(temporary)],check=True)
        os.replace(temporary,output)
    print(f'VMware: {output}, capacity {size_mib} MiB; BFS4 partition {size_mib-1} MiB')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('mbr',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--size-mib',type=int,default=4096);p.add_argument('--qemu-img',default='qemu-img')
    a=p.parse_args();build(a.source,a.mbr,a.output,a.size_mib,a.qemu_img)


if __name__=='__main__':main()
