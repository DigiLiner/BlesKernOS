#!/usr/bin/env python3
"""Stage a verified PIC checkpoint in a new BFS4 test clone, preserving other files."""
import argparse,hashlib,json,shutil,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--work',type=Path,required=True)
p.add_argument('--patchelf',default='patchelf',help='ELF packaging tool; may be extracted in a private directory')
a=p.parse_args();base=a.base.resolve();output=a.output.resolve();work=a.work.resolve()
if output.exists():p.error('output must be a new disposable image')
for process in Path('/proc').iterdir():
    if not process.name.isdecimal():continue
    try:
        if 'qemu-system' in (process/'comm').read_text() and any(fd.resolve()==base for fd in (process/'fd').iterdir()):
            p.error('stop the VM using the base image before cloning it')
    except OSError:pass
manifest=json.loads((work/'manifest.json').read_text())
checkpoint=json.loads((work/'priority-completed.json').read_text())
if not manifest.get('memory_pic'):p.error('PIC build manifest required')
expected=json.loads((ROOT/'ports/wine/versions/wine-11.0.json').read_text())
if manifest['source']['source_sha256']!=expected['source_sha256']:p.error('unexpected Wine source')
patch=ROOT/'ports/wine/patches/0008-i386-pic-memory.patch'
if not any(v['sha256']==hashlib.sha256(patch.read_bytes()).hexdigest() for v in manifest['patches']):p.error('PIC patch differs from manifest')
if '#define SONAME_LIBGL ' not in (work/'obj/include/config.h').read_text():p.error('existing OpenGL backend required')
def sha(data):return hashlib.sha256(data).hexdigest()
payloads={};report={'scope':'experimental complete modules observed in WinZip; full upstream build still required','source':str(base),'build':manifest,'files':{}}
report['patchelf_version']=subprocess.check_output([a.patchelf,'--version'],text=True).strip()
with tempfile.TemporaryDirectory(prefix='bk-wine-pic-stage-') as folder:
    for target,digest in checkpoint['hashes'].items():
        src=work/'obj'/target;raw=src.read_bytes()
        if sha(raw)!=digest or raw[:5]!=b'\x7fELF\x01':raise ValueError('checkpoint ELF mismatch: '+target)
        name=src.name
        guest='/OPT/WINE/bin/'+name if target in ('tools/wine/wine','server/wineserver') else '/OPT/WINE/lib/wine/i386-unix/'+name
        staged=Path(folder)/name;staged.write_bytes(raw)
        header=subprocess.check_output(['readelf','-l',str(staged)],text=True)
        if 'INTERP' in header:subprocess.run([a.patchelf,'--set-interpreter','/WINEGATE/lib/ld-linux.so.2',str(staged)],check=True)
        subprocess.run([a.patchelf,'--add-rpath','/WINEGATE/lib',str(staged)],check=True)
        data=staged.read_bytes();dynamic=subprocess.check_output(['readelf','-d',str(staged)],text=True)
        payloads[guest]=data
        report['files'][guest]={'source_sha256':digest,'staged_sha256':sha(data),'bytes':len(data),'textrel':'(TEXTREL)' in dynamic,'dynamic':dynamic}
output.parent.mkdir(parents=True,exist_ok=True)
temp=output.with_suffix(output.suffix+'.pic-staging')
if temp.exists():p.error('staging temporary image already exists')
subprocess.run(['cp','--reflink=auto',str(base),str(temp)],check=True)
try:
    editor=Editor(temp);before={i:tuple(v) for i,v in editor.inodes.items()};changed=set()
    try:
        for guest,data in sorted(payloads.items()):
            inode=editor.find(guest)  # Only replace already installed modules.
            changed.add(inode);editor.update(guest,data)
        assert all(tuple(editor.inodes[i])==v for i,v in before.items() if i not in changed)
        editor.finish()
    finally:editor.fd.close()
    bfs4.check(temp)
    report['preserved_inodes']=len(before)-len(changed)
    temp.replace(output)
except BaseException:
    temp.unlink(missing_ok=True);raise
output.with_suffix(output.suffix+'.json').write_text(json.dumps(report,indent=2))
print('Experimental PIC test seed:',output,'; updated',len(changed),'modules')
