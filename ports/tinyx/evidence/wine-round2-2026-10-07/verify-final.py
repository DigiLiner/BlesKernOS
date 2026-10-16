from pathlib import Path
import hashlib,json,struct
out=Path('ports/tinyx/evidence/wine-round2-2026-10-07')
a=Path('build/wine-stable-before-round2/ata.img');b=Path('build/bleskernos-ata-user.img')
changed=0
with a.open('rb') as old,b.open('rb') as new:
 pos=0
 while True:
  x=old.read(4096);y=new.read(4096)
  if not x:assert not y;break
  assert len(x)==len(y)
  if x!=y:
   changed+=1
   for i,(p,q) in enumerate(zip(x,y)):
    if p!=q:assert 512<=pos+i<4096*512,(pos+i,'filesystem byte changed')
  pos+=len(x)
kernel=Path('build/kernel.bin').read_bytes();stage=Path('build/stage2.bin').read_bytes()
for p,offset in [(b,0),(Path('build/bleskernos-usb-user.img'),2048*512)]:
 with p.open('rb') as f:
  f.seek(offset+512);assert f.read(len(stage))==stage
  f.seek(offset+9*512);assert f.read(len(kernel))==kernel
checks={'changed_blocks':changed,'filesystem_bytes_preserved':True,'ata_and_usb_embed_current_kernel':True,'sha256':{}}
for p in [a,b,Path('build/bleskernos-usb-user.img'),Path('build/bleskernos-vmware-user.vmdk'),Path('build/kernel.bin')]:
 h=hashlib.sha256()
 with p.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 checks['sha256'][str(p)]=h.hexdigest()
checks['baseline']={'ide_wine_ms':56070,'ahci_wine_ms':14710}
checks['final_samples']={'ide_wine_ms':[14000,15500],'ahci_wine_ms':[11540,14310],'ide_first_surface_ms':[2190,2660],'ahci_first_surface_ms':[2150,2890]}
(out/'result.json').write_text(json.dumps(checks,indent=2)+'\n')
print('Preserved all filesystem bytes; current kernel embedded in ATA/USB;',changed,'changed boot blocks')
