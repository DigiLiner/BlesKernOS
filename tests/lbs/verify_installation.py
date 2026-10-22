#!/usr/bin/env python3
"""Verify installed bytes/layout and preserve original boot/user/Wine data."""
import argparse,hashlib,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor
from lbs_kernel import expected_boot
from lbs_payload import application_payloads, documentation_payloads
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--kernel-dir',type=Path,default=ROOT/'build')
p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-lbs-user.img');a=p.parse_args()
source,installed=Editor(a.source),Editor(a.image)
files=application_payloads()
files.update(documentation_payloads())
files['/SYSTEM/SERVICES/VISION.BEX']=ROOT/'build/system/services/vision.o'
files['/SYSTEM/DRIVERS/TLS.DVR']=ROOT/'build/system/drivers/TLS.DVR'
for lib in (ROOT/'build/system/libs/native').glob('*.LBS'):files['/SYSTEM/'+lib.name]=lib
for guest,host in [('LIBLBS.A','libblesk_lbs.a'),('LIBPOSIX.A','libblesk_posix.a'),('LIBBLESK.A','libblesk.a'),('LBSHOST.O','lbs_host.o'),('LIBLBSPX.A','libblesk_lbs_posix.a'),('LIBLBSNT.A','libblesk_lbs_native.a')]:files['/SYSTEM/SDK/LIB/'+guest]=ROOT/'build/sdk'/host
for guest,host in [('LBSTRI.BEX','build/tests/userland/LBSTRI.BEX'),('NETWORK_STATUS.BEX','build/lbs/apps/programs/network_status.o')]:files['/SYSTEM/PROGRAMS/'+guest]=ROOT/host
for guest,host in [('LBS.TXT','LBS.md'),('LBSREPORT.TXT','LBS-REPORT.md'),('LBSMIG.TXT','LBS-MIGRATION.md'),('LBSSHARE.TXT','LBS-SHARING.md'),('LBSEXPORT.TXT','LBS-EXPORTS.md')]:files['/SYSTEM/DOCS/'+guest]=ROOT/'docs/native'/host
for header in (ROOT/'sdk/include').glob('*.h'):files['/SYSTEM/SDK/INCLUDE/'+header.name.upper()]=header
preserved=[];hashes={}
try:
 for guest,host in files.items():
  data=host.read_bytes();assert installed.contents(guest)==data,guest
  hashes[guest]=hashlib.sha256(data).hexdigest()
 for lib in (ROOT/'build/system/libs/native').glob('*.LBS'):
  try:installed.find('/SYSTEM/LIBS/NATIVE/'+lib.name)
  except FileNotFoundError:pass
  else:raise AssertionError('obsolete library copy: '+lib.name)
 def inode_path(i):
  names=[]
  while i!=1:
   v=source.inodes[i];names.append(v[27].split(b'\0')[0].decode());i=v[1]
  return '/'+ '/'.join(reversed(names))
 for i,v in source.inodes.items():
  if v[3]&0o170000!=bfs4.MODE_FILE:continue
  path=inode_path(i)
  if path.upper().startswith(('/SYSTEM/USER/','/WINEGATE/PREFIX/')):
   assert source.contents(path)==installed.contents(path),path
   preserved.append(path)
 with a.source.open('rb') as x,a.image.open('rb') as y:
  original_boot=x.read(bfs4.BOOT_SECTORS*512);boot=expected_boot(original_boot,a.kernel_dir);assert boot==y.read(len(boot));assert boot[:512]==original_boot[:512]
 result={'status':'pass','installed_image':str(a.image),'boot_area_sha256':hashlib.sha256(boot).hexdigest(),'kernel_updated':True,'stage1_preserved':True,
         'checked_files_sha256':hashes,'preserved_user_and_wine_files':preserved,'old_library_copies':0}
 output=ROOT/'tests/lbs/evidence/migration/installation.json';output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(result,indent=2)+'\n')
 print(f'PASS: {len(files)} installed payloads, {len(preserved)} user/Wine files preserved, expected kernel/stage 2 and identical stage 1, no obsolete library copies')
finally:source.fd.close();installed.fd.close()
