# Reproduction helper for halted copies of this specific ATA BFS4 image.
# Run from the repository root; does not allocate/repack files.
from pathlib import Path
import sys,struct,time,subprocess
sys.path.insert(0,'tools')
import bfs4
image=Path(sys.argv[1]); launcher=Path('build/wine/wine-launch').read_bytes()
with image.open('r+b') as f:
 start=4096*512;f.seek(start);superblock=struct.unpack(bfs4.SUPER_FORMAT,f.read(struct.calcsize(bfs4.SUPER_FORMAT)));assert superblock[0]==bfs4.MAGIC
 table=start+superblock[9]*4096
 for i in range(1,superblock[11]):
  f.seek(table+i*256);raw=f.read(256);v=list(struct.unpack(bfs4.INODE_FORMAT,raw))
  if v[27].split(b'\0')[0] not in (b'wine-launch',b'wine-launch.ELF'):continue
  assert v[3]&0o170000==0o100000
  ext=[(v[10+j*2],v[11+j*2]) for j in range(min(v[9],8))]
  assert v[9]<=8 and sum(n for _,n in ext)*4096>=len(launcher)
  data=launcher+bytes(sum(n for _,n in ext)*4096-len(launcher));pos=0
  for b,n in ext:f.seek(start+b*4096);f.write(data[pos:pos+n*4096]);pos+=n*4096
  v[2]=len(launcher);v[6]=v[7]=int(time.time());v[28]=0
  raw=bytearray(struct.pack(bfs4.INODE_FORMAT,*v));struct.pack_into('<I',raw,248,bfs4.crc(raw));f.seek(table+i*256);f.write(raw)
  print('updated',i,v[27].split(b'\0')[0])
 kernel=Path('build/kernel.bin').read_bytes();assert len(kernel)<=1280*1024
 stage=Path('/tmp/blesk-fd-neutral/stage2.bin')
 subprocess.run(['nasm','-f','bin',f'-DKERNEL_SECTORS={(len(kernel)+511)//512}',f'-DKERNEL_CHECKSUM={sum(kernel)&0xffffffff}','boot/stage2.asm','-o',str(stage)],check=True)
 f.seek(512);f.write(stage.read_bytes());f.seek(9*512);f.write(kernel);f.write(bytes((-len(kernel))%512))
