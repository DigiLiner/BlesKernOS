from pathlib import Path
import shutil,os
source=Path('build/wine-stable-before-round2/ata.img')
target=Path('build/bleskernos-ata-user.img')
temporary=target.with_suffix('.img.new')
kernel=Path('build/kernel.bin').read_bytes();stage2=Path('build/stage2.bin').read_bytes()
assert len(kernel)<=1280*1024 and len(stage2)==8*512
shutil.copyfile(source,temporary)
with temporary.open('r+b') as f:
 f.seek(4096*512);assert f.read(4)==b'BFS4'
 f.seek(512);f.write(stage2)
 f.seek(9*512);f.write(kernel);f.write(bytes((-len(kernel))%512))
changed=0
with source.open('rb') as a,temporary.open('rb') as b:
 position=0
 while True:
  old=a.read(4096);new=b.read(4096)
  if not old:assert not new;break
  assert len(old)==len(new)
  if old!=new:
   changed+=1
   for i,(x,y) in enumerate(zip(old,new)):
    if x!=y:assert 512<=position+i<4096*512,(position+i,'unexpected user-data modification')
  position+=len(old)
os.replace(temporary,target)
print('Patched only boot reservation; all filesystem bytes preserved;',changed,'changed blocks')
