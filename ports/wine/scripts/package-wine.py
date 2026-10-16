#!/usr/bin/env python3
"""Paquete instalado Wine 11.0 + cierre ELF i386, hashes y licencias; sin afirmar gates guest."""
import argparse,hashlib,json,re,shutil,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def output(cmd):return subprocess.check_output([str(x) for x in cmd],text=True,stderr=subprocess.STDOUT)
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume',action='store_true');a=p.parse_args()
w=a.work.resolve();dest=a.output.resolve()
if dest.exists() and any(dest.iterdir()) and not a.resume:p.error('usar destino nuevo o --resume')
manifest=json.loads((w/'manifest.json').read_text())
if manifest.get('build') not in ('completed','completed; dependencias de runtime externas aun requieren empaquetado'):p.error('build/install upstream no completado')
root=dest/'rootfs';root.mkdir(parents=True,exist_ok=True)
if not a.resume:shutil.copytree(w/'stage',root,symlinks=True,dirs_exist_ok=True)
licenses=dest/'licenses';licenses.mkdir(exist_ok=True);shutil.copyfile(w/'wine-wine-11.0/LICENSE',licenses/'Wine-LICENSE');shutil.copyfile(w/'wine-wine-11.0/AUTHORS',licenses/'Wine-AUTHORS')
metadata=dest/'build';metadata.mkdir(exist_ok=True)
for name in ['manifest.json','configure.log','configure-help.txt','resume.log','invalidated-elf64-imports.json']:
 if (w/name).exists():shutil.copyfile(w/name,metadata/name)
shutil.copyfile(w/'obj/config.log',metadata/'config.log')
for name in ['as','ld']:shutil.copyfile(w/'toolchain32'/name,metadata/('toolchain-'+name))
(metadata/'dependencies.txt').write_text(output(['dpkg-query','-W','-f=${binary:Package}\t${Version}\n']))
libs=root/'WINEGATE/lib';libs.mkdir(parents=True,exist_ok=True)
def external(name):
 return next((Path(d)/name for d in ['/usr/lib32','/lib32','/usr/lib/i386-linux-gnu','/lib/i386-linux-gnu'] if (Path(d)/name).is_file()),None)
# These are loaded with dlopen, so an ELF NEEDED closure alone misses them.
# The queue below also copies their FreeType/Fontconfig dependencies.
for name in ['libgcc_s.so.1','libfreetype.so.6','libfontconfig.so.1']:
 candidate=external(name)
 if candidate is None:raise RuntimeError('runtime dlopen sin resolver: '+name)
 if not (libs/name).exists():shutil.copyfile(candidate,libs/name)
fonts=root/'WINEGATE/etc/fonts';fonts.mkdir(parents=True,exist_ok=True)
(fonts/'fonts.conf').write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>/WINEGATE/share/fonts</dir>
  <cachedir>/WINEGATE/cache/fontconfig</cachedir>
</fontconfig>
''')
(root/'WINEGATE/share/fonts').mkdir(parents=True,exist_ok=True)
(root/'WINEGATE/cache/fontconfig').mkdir(parents=True,exist_ok=True)
queue=[p for p in root.rglob('*') if p.is_file() and not p.is_symlink()];seen=set();files={};packages=set()
while queue:
 file=queue.pop()
 if file in seen:continue
 seen.add(file)
 if a.resume:
  pristine=external(file.name) if file.parent==libs else w/'stage'/file.relative_to(root)
  if pristine and pristine.is_file():shutil.copyfile(pristine,file)
 with file.open('rb') as f:header=f.read(5)
 if header[:4]!=b'\x7fELF':continue
 if header[4]!=1:raise RuntimeError('ELF no i386: '+str(file))
 original=sha(file);dynamic=output(['readelf','-d',file]);needed=re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic)
 changes=[]
 if file.parent!=libs and 'INTERP' in output(['readelf','-l',file]):
  subprocess.run(['patchelf','--set-interpreter','/WINEGATE/lib/ld-linux.so.2',file],check=True);changes.append('PT_INTERP=/WINEGATE/lib/ld-linux.so.2')
 if 'Dynamic section' in dynamic and file.parent!=libs:
  subprocess.run(['patchelf','--add-rpath','/WINEGATE/lib',file],check=True);changes.append('RUNPATH += /WINEGATE/lib')
 files[str(file.relative_to(root))]={'source_sha256':original,'packaged_sha256':sha(file),'needed':needed,'packaging':changes}
 for name in needed+(['ld-linux.so.2'] if changes else []):
  if (root/'opt/wine/lib/wine/i386-unix'/name).is_file():continue
  target=libs/name
  if target.exists():continue
  candidate=external(name)
  if candidate is None:raise RuntimeError('dependencia sin resolver: '+name)
  shutil.copyfile(candidate,target);queue.append(target)
  for spelling in {str(candidate),str(candidate.resolve()),str(candidate).replace('/usr/lib/','/lib/')}:
   query=subprocess.run(['dpkg-query','-S',spelling],text=True,capture_output=True)
   if not query.returncode:
    packages.add(query.stdout.split(': ')[0]);break
for library in libs.iterdir():
 candidate=external(library.name)
 if not candidate:raise RuntimeError('origen de runtime desconocido: '+library.name)
 if sha(library)!=sha(candidate):raise RuntimeError('runtime externo modificado: '+library.name)
 for spelling in {str(candidate),str(candidate.resolve()),str(candidate).replace('/usr/lib/','/lib/')}:
  query=subprocess.run(['dpkg-query','-S',spelling],text=True,capture_output=True)
  if not query.returncode:packages.add(query.stdout.split(': ')[0]);break
for package in sorted(packages):
 name=package.split(':')[0];copy=Path('/usr/share/doc')/name/'copyright'
 if copy.is_file():shutil.copyfile(copy,licenses/(package.replace(':','-')+'-copyright'))
links={str(p.relative_to(root)):str(p.readlink()) for p in root.rglob('*') if p.is_symlink()}
(dest/'manifest.json').write_text(json.dumps({'wine':manifest,'guest_gates':'W1 validado con bootstrap; paquete completo aun no ejecutado','elf':files,'symlinks':links,'runtime_packages':sorted(packages)},indent=2)+'\n')
allhash={str(p.relative_to(dest)):sha(p) for p in dest.rglob('*') if p.is_file() and not p.is_symlink() and p!=dest/'sha256.json'}
(dest/'sha256.json').write_text(json.dumps(allhash,indent=2)+'\n')
archive=dest.parent/(dest.name+'.tar.gz')
with tarfile.open(archive,'w:gz') as tar:tar.add(dest,arcname=dest.name)
archive.with_suffix(archive.suffix+'.sha256').write_text(sha(archive)+'  '+archive.name+'\n')
print(f'Paquete completo: {archive}; {len(files)} ELF; {len(links)} symlinks')
