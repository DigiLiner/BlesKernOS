#!/usr/bin/env python3
"""Check USB partition identity and every rebuilt VMware BFS4 inode/file."""
import argparse, hashlib, json, struct, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor

def digest(stream):
    h=hashlib.sha256()
    while data:=stream.read(1024*1024):h.update(data)
    return h.hexdigest()

def paths(editor):
    names={1:'/'}
    def name(i):
        if i not in names:
            v=editor.inodes[i]
            names[i]=name(v[1]).rstrip('/')+'/'+v[27].split(b'\0')[0].decode()
        return names[i]
    return {name(i):v for i,v in editor.inodes.items()}

def partition(path):
    with path.open('rb') as stream:
        mbr=stream.read(512)
    assert mbr[510:]==b'\x55\xaa'
    entry=mbr[446:462]
    assert entry[0]==0x80 and entry[4]==0x83
    lba,sectors=struct.unpack_from('<II',entry,8)
    assert lba==2048
    assert (lba+sectors)*512==path.stat().st_size
    return lba*512

def reader(path,offset):
    e=Editor.__new__(Editor)
    e.fd=path.open('rb');e.base=offset+bfs4.BOOT_SECTORS*512
    e.fd.seek(e.base)
    e.super=list(struct.unpack(bfs4.SUPER_FORMAT,e.fd.read(struct.calcsize(bfs4.SUPER_FORMAT))))
    assert e.super[0]==bfs4.MAGIC
    e.imap=e.read(e.super[7],e.super[8]*bfs4.BLOCK)
    e.table=e.read(e.super[9],e.super[10]*bfs4.BLOCK)
    e.inodes={i:list(struct.unpack(bfs4.INODE_FORMAT,e.table[i*256:(i+1)*256]))
              for i in range(1,e.super[11]) if bfs4.bit_get(e.imap,i)}
    return e

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=ROOT/'build/bleskernos-lbs-user.img')
p.add_argument('--usb',type=Path,default=ROOT/'build/bleskernos-usb-user.img')
p.add_argument('--vmware',type=Path,default=ROOT/'build/bleskernos-vmware-user.vmdk')
a=p.parse_args()
offset=partition(a.usb)
with a.source.open('rb') as src,a.usb.open('rb') as usb:
    usb.seek(offset)
    boot=bytearray(src.read(512));boot[28:32]=(offset//512).to_bytes(4,'little')
    expected=hashlib.sha256(boot)
    while data:=src.read(1024*1024):expected.update(data)
    original=expected.hexdigest();assert digest(usb)==original
info=json.loads(subprocess.check_output(['qemu-img','info','--output=json',str(a.vmware)],text=True))
assert info['format']=='vmdk' and info['virtual-size']==4096*1024*1024
with tempfile.TemporaryDirectory(prefix='lbs-delivery-',dir=ROOT/'build') as work:
    raw=Path(work)/'vmware.raw'
    subprocess.run(['qemu-img','convert','-f','vmdk','-O','raw',str(a.vmware),str(raw)],check=True)
    offset=partition(raw)
    bfs4.check(raw,boot_sectors=bfs4.BOOT_SECTORS+offset//512)
    source=Editor(a.source);target=reader(raw,offset)
    try:
        old,new=paths(source),paths(target)
        assert old.keys()==new.keys()
        files=0;byte_count=0
        for guest,prior in old.items():
            current=new[guest]
            assert prior[2]==current[2] and prior[3:9]==current[3:9],guest
            if prior[3]&0o170000!=bfs4.MODE_DIR:
                assert source.contents(guest)==target.contents(guest),guest
                files+=1;byte_count+=prior[2]
    finally:source.fd.close();target.fd.close()
result={'status':'pass','source':str(a.source),'source_partition_sha256':original,
        'usb':str(a.usb),'usb_partition_identical_except_required_bpb_hidden_sectors':True,'vmware':str(a.vmware),
        'vmware_capacity_mib':4096,'vmware_partition_mib':4095,
        'vmware_all_inodes_checked':len(old),'vmware_files_checked':files,
        'vmware_file_bytes_checked':byte_count,'modes_owners_timestamps_preserved':True}
output=ROOT/'tests/lbs/evidence/total/delivery.json'
output.write_text(json.dumps(result,indent=2)+'\n')
print('PASS: identical USB partition; VMware',files,'files and all inode metadata, 4096 MiB')
