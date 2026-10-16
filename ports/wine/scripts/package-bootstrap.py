#!/usr/bin/env python3
"""Empaqueta el bootstrap real de Wine 11.0 para W1/W2; no es una instalacion completa."""
import argparse,hashlib,json,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--output',type=Path,default=ROOT/'ports/wine/rootfs');a=p.parse_args()
w=a.work.resolve();out=a.output.resolve();dest=out/'WINEGATE/wine';dest.mkdir(parents=True,exist_ok=True)
meta=ROOT/'ports/wine/build/upstream-bootstrap';meta.mkdir(parents=True,exist_ok=True)
src=w/'wine-wine-11.0';files={}
for relative,name in [('loader/wine','wine'),('dlls/ntdll/ntdll.so','ntdll.so'),('server/wineserver','wineserver')]:
 original=w/'obj'/relative;target=dest/name;shutil.copyfile(original,target)
 target.chmod(0o755)
 commands=[]
 if name!='ntdll.so':
  command=['patchelf','--set-interpreter','/WINEGATE/lib/ld-linux.so.2',str(target)];subprocess.run(command,check=True);commands.append(command)
 command=['patchelf','--set-rpath','/WINEGATE/lib',str(target)];subprocess.run(command,check=True);commands.append(command)
 files[name]={'original_sha256':sha(original),'packaged_sha256':sha(target),'packaging_commands':commands,'needed':subprocess.check_output(['readelf','-d',target],text=True)}
for name in ['LICENSE','AUTHORS']:
 shutil.copyfile(src/name,meta/name)
for name in ['manifest.json','configure.log','configure-help.txt']:
 shutil.copyfile(w/name,meta/name)
shutil.copyfile(w/'obj/config.log',meta/'config.log')
(meta/'dependencies.txt').write_text(subprocess.check_output(['dpkg-query','-W','-f=${binary:Package}\t${Version}\n'],text=True))
(meta/'bootstrap.json').write_text(json.dumps({'version':'11.0','scope':'Wine upstream ELF bootstrap; no PE DLL installation yet','patches':[],'files':files,'guest_result':'PENDING'},indent=2)+'\n')
command=['gcc','-m32','-O2','-no-pie','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,-rpath,/WINEGATE/lib',str(ROOT/'ports/wine/tests/wine-launch.c'),'-o',str(out/'WINEGATE/bin/wine-launch')]
subprocess.run(command,check=True)
print('Bootstrap upstream empaquetado: '+str(dest))
