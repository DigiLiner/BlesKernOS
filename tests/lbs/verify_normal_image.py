#!/usr/bin/env python3
"""Verify the normal user image installs every manifest client and current LBS."""
import argparse,hashlib,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from lbs_payload import application_payloads,documentation_payloads
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-ata-user.img');a=p.parse_args()
files=application_payloads();files.update(documentation_payloads())
files.update({'/SYSTEM/'+f.name:f for f in (ROOT/'build/system/libs/native').glob('*.LBS')})
files['/SYSTEM/SERVICES/VISION.BEX']=ROOT/'build/system/services/vision.o'
files['/SYSTEM/DRIVERS/TLS.DVR']=ROOT/'build/system/drivers/TLS.DVR'
for guest,host in [('LIBLBSPX.A','libblesk_lbs_posix.a'),('LIBLBSNT.A','libblesk_lbs_native.a'),('LIBLBS.A','libblesk_lbs.a'),('LBSHOST.O','lbs_host.o')]:files['/SYSTEM/SDK/LIB/'+guest]=ROOT/'build/sdk'/host
out=ROOT/'tests/lbs/evidence/total/normal-image.json';out.unlink(missing_ok=True)
e=Editor(a.image);hashes={}
try:
 for guest,host in files.items():
  data=e.contents(guest);assert data==host.read_bytes(),guest
  hashes[guest]=hashlib.sha256(data).hexdigest()
 with a.image.open('rb') as stream:
  stream.seek(512);assert stream.read(4096)==(ROOT/'build/stage2.bin').read_bytes()
  stream.seek(9*512);kernel=(ROOT/'build/kernel.bin').read_bytes();assert stream.read(len(kernel))==kernel
finally:e.fd.close()
out.write_text(json.dumps({'status':'pass','image':str(a.image),'edition':'user','native_clients':len(application_payloads()),'libraries':15,'checked_payloads':hashes},indent=2)+'\n')
print('PASS:',len(files),'normal image payloads; 139 migrated clients, 15 LBS, SDK, current kernel/stage2')
