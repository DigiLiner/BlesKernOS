#!/usr/bin/env python3
"""Update files in a stopped BFS4 image without repacking user data or modes.
Writes a checked temporary image and atomically replaces the specified image.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import time
import bfs4 as bfs
from bfs4_journal import require_checkpoint

class Editor:
    def __init__(self,path):
        self.fd=Path(path).open('r+b');self.base=bfs.BOOT_SECTORS*512
        try:require_checkpoint(self.fd)
        except Exception:
            self.fd.close();raise
        self.fd.seek(self.base);self.super=list(struct.unpack(bfs.SUPER_FORMAT,self.fd.read(struct.calcsize(bfs.SUPER_FORMAT))))
        assert self.super[0]==bfs.MAGIC
        self.bitmap=self.read(self.super[5],self.super[6]*bfs.BLOCK)
        self.imap=self.read(self.super[7],self.super[8]*bfs.BLOCK)
        self.table=self.read(self.super[9],self.super[10]*bfs.BLOCK)
        self.inodes={i:list(struct.unpack(bfs.INODE_FORMAT,self.table[i*256:(i+1)*256])) for i in range(1,self.super[11]) if bfs.bit_get(self.imap,i)}
    def read(self,block,size):
        self.fd.seek(self.base+block*bfs.BLOCK);return bytearray(self.fd.read(size))
    def write(self,block,data):
        self.fd.seek(self.base+block*bfs.BLOCK);self.fd.write(data)
    def find(self,path):
        parent=1
        for component in Path(path).parts[1:]:
            matches=[i for i,v in self.inodes.items() if v[1]==parent and v[27].split(b'\0')[0].decode().casefold()==component.casefold()]
            if len(matches)!=1:raise FileNotFoundError(path)
            parent=matches[0]
        return parent
    def extents(self,v):
        result=[tuple(v[10+2*i:12+2*i]) for i in range(min(v[9],8))]
        if v[9]>8:
            data=self.read(v[26],bfs.BLOCK)
            result += [struct.unpack_from('<II',data,(i-8)*8) for i in range(8,v[9])]
        return result
    def contents(self,path):
        v=self.inodes[self.find(path)];return b''.join(self.read(b,n*bfs.BLOCK) for b,n in self.extents(v))[:v[2]]
    def update(self,path,data):
        name=Path(path).name;parent=self.find(str(Path(path).parent))
        try:i=self.find(path);v=self.inodes[i]
        except FileNotFoundError:
            i=next(i for i in range(2,self.super[11]) if not bfs.bit_get(self.imap,i))
            v=[i,parent,0,bfs.MODE_FILE|0o755,0,*([int(time.time())]*3),1,0,*([0]*16),0,name.encode().ljust(128,b'\0'),0,0]
            bfs.bit_set(self.imap,i)
        if v[3]&0o170000!=bfs.MODE_FILE:raise ValueError('only regular files can be updated: '+path)
        old=self.extents(v);blocks=(len(data)+bfs.BLOCK-1)//bfs.BLOCK
        capacity=sum(n for _,n in old)
        if capacity>=blocks:
            extents=[];remaining=blocks
            for start,count in old:
                keep=min(count,remaining)
                if keep:extents.append((start,keep));remaining-=keep
                for j in range(start+keep,start+count):bfs.bit_set(self.bitmap,j,False)
            v[9]=len(extents)
            v[10:26]=[j for pair in (extents[:8]+[(0,0)]*8)[:8] for j in pair]
            if len(extents)>8:
                indirect=bytearray(bfs.BLOCK)
                for index,pair in enumerate(extents[8:]):struct.pack_into('<II',indirect,index*8,*pair)
                self.write(v[26],indirect)
            elif v[26]:
                bfs.bit_set(self.bitmap,v[26],False);v[26]=0
        else:
            begin=None;count=0
            for b in range(self.super[13],self.super[3]):
                if not bfs.bit_get(self.bitmap,b):
                    if not count:begin=b
                    count+=1
                    if count==blocks:break
                else:count=0
            if count!=blocks:raise ValueError('no contiguous free extent')
            extents=[(begin,blocks)]
            for b in range(begin,begin+blocks):bfs.bit_set(self.bitmap,b)
            for b,n in old:
                for j in range(b,b+n):bfs.bit_set(self.bitmap,j,False)
            if v[26]:bfs.bit_set(self.bitmap,v[26],False)
            v[9]=len(extents);v[10:26]=[j for pair in (extents+[(0,0)]*7) for j in pair];v[26]=0
        position=0
        for b,n in extents:
            size=n*bfs.BLOCK;self.write(b,data[position:position+size].ljust(size,b'\0'));position+=size
        v[2]=len(data);v[6]=v[7]=int(time.time());v[28]=0
        raw=bytearray(struct.pack(bfs.INODE_FORMAT,*v));struct.pack_into('<I',raw,248,bfs.crc(raw))
        self.table[i*256:(i+1)*256]=raw;self.inodes[i]=v
        print(path,len(data),'bytes; mode',oct(v[3]))
    def mkdir(self,path):
        """Create missing parent directories; BFS4 stores children by inode parent."""
        path=Path(path)
        if str(path)=='/':return 1
        try:
            i=self.find(str(path))
            if self.inodes[i][3]&0o170000!=bfs.MODE_DIR:
                raise ValueError('directory path is a file: '+str(path))
            return i
        except FileNotFoundError:pass
        parent=self.mkdir(str(path.parent))
        name=path.name.encode()
        if not name or len(name)>127:raise ValueError('invalid BFS4 directory name')
        i=next(i for i in range(2,self.super[11]) if not bfs.bit_get(self.imap,i))
        v=[i,parent,0,bfs.MODE_DIR|0o755,0,*([int(time.time())]*3),1,0,*([0]*16),0,name.ljust(128,b'\0'),0,0]
        bfs.bit_set(self.imap,i)
        raw=bytearray(struct.pack(bfs.INODE_FORMAT,*v))
        struct.pack_into('<I',raw,248,bfs.crc(raw))
        self.table[i*256:(i+1)*256]=raw;self.inodes[i]=v
        return i
    def remove(self,path):
        try: root=self.find(path)
        except FileNotFoundError: return
        if root==1: raise ValueError('cannot remove filesystem root')
        doomed={root}
        while True:
            children={i for i,v in self.inodes.items() if v[1] in doomed}
            if children <= doomed: break
            doomed |= children
        for i in doomed:
            v=self.inodes.pop(i)
            for block,count in self.extents(v):
                for j in range(block,block+count): bfs.bit_set(self.bitmap,j,False)
            if v[26]: bfs.bit_set(self.bitmap,v[26],False)
            bfs.bit_set(self.imap,i,False)
            self.table[i*256:(i+1)*256]=bytes(256)
        print('removed',path,len(doomed),'entries')
    def finish(self):
        self.write(self.super[5],self.bitmap);self.write(self.super[7],self.imap);self.write(self.super[9],self.table)
        self.super[4]=sum(not bfs.bit_get(self.bitmap,i) for i in range(self.super[3]))
        self.super[-1]=0;raw=bytearray(struct.pack(bfs.SUPER_FORMAT,*self.super));struct.pack_into('<I',raw,len(raw)-4,bfs.crc(raw))
        self.write(0,raw);self.fd.flush();os.fsync(self.fd.fileno());self.fd.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('image',type=Path);p.add_argument('files',nargs='*',help='/guest/path=host/path')
    p.add_argument('--remove',action='append',default=[],help='remove a guest path and its descendants')
    a=p.parse_args();temporary=a.image.with_suffix(a.image.suffix+'.update')
    if temporary.exists():p.error('temporary file already exists')
    bfs.check(a.image);shutil.copyfile(a.image,temporary)
    try:
        e=Editor(temporary)
        for path in a.remove: e.remove(path)
        for item in a.files:
            guest,host=item.split('=',1);e.update(guest,Path(host).read_bytes())
        e.finish();bfs.check(temporary);os.replace(temporary,a.image)
    finally:
        temporary.unlink(missing_ok=True)
if __name__=='__main__':main()
