#!/usr/bin/env python3
"""Apply quiet Wine/Vision payloads to a backed-up offline image atomically."""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
import bfs4
from bfs4_update import Editor
from bfs4_journal import recover
from wine_desktop_defaults import desktop_defaults
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--winzip-test',action='store_true',help='replace the memory monitor startup with normal WinZip launch')
p.add_argument('--kernel-dir',type=Path,help='install the validated kernel.bin and stage2.bin from this directory')
a=p.parse_args();image=a.image.resolve()
kernel=stage=None
if a.kernel_dir:
    kernel=(a.kernel_dir/'kernel.bin').read_bytes()
    stage=(a.kernel_dir/'stage2.bin').read_bytes()
    if not kernel or len(kernel)>1280*1024 or len(stage)!=4096:
        p.error('invalid kernel/stage2 size for the reserved boot area')
for proc in Path('/proc').iterdir():
    if not proc.name.isdecimal():continue
    try:
        if 'qemu-system' in (proc/'comm').read_text() and any(fd.resolve()==image for fd in (proc/'fd').iterdir()):
            p.error('stop QEMU using this image before updating it')
    except OSError:pass
backup=image.with_name(image.stem+'.before-quiet-'+time.strftime('%Y%m%d-%H%M%S')+image.suffix)
temp=image.with_suffix(image.suffix+'.quiet-update')
if temp.exists() or backup.exists():p.error('update/backup already exists')
subprocess.run(['cp','--reflink=auto',str(image),str(backup)],check=True)
report={'backup':str(backup),'kernel_changed':False,'files':[]}
try:
    subprocess.run(['cp','--reflink=auto',str(image),str(temp)],check=True)
    report['recovered_journal_records']=recover(temp)
    e=Editor(temp)
    try:
        before={i:tuple(v) for i,v in e.inodes.items()};changed=set()
        def put(guest,data):
            try:changed.add(e.find(guest))
            except FileNotFoundError:pass
            e.update(guest,data);report['files'].append(guest)
        for guest,source in [('/WINEGATE/bin/wine-launch',ROOT/'build/wine/wine-launch'),('/WINEGATE/bin/wine-launch.ELF',ROOT/'build/wine/wine-launch'),('/WINEGATE/bin/wine-ready.exe',ROOT/'build/wine/wine-ready.exe'),('/SYSTEM/SERVICES/VISION.BEX',ROOT/'build/system/services/vision.o')]:
            put(guest,source.read_bytes())
        put('/WINEGATE/prefix/user.reg',desktop_defaults(e.contents('/WINEGATE/prefix/user.reg')))
        if a.winzip_test:
            e.find('/SYSTEM/TEST/WZ8/WINZIP32.EXE')
            put('/SYSTEM/PROGRAMS/WINE.BEX',(ROOT/'build/programs/wine.o').read_bytes())
            put('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|WinZip|/SYSTEM/PROGRAMS/WINE.BEX|/SYSTEM/TEST/WZ8/WINZIP32.EXE|Normal Wine launch\n')
        assert all(tuple(e.inodes[i])==v for i,v in before.items() if i not in changed)
        if kernel is not None:
            if e.base<9*512+1280*1024:raise ValueError('boot payload overlaps the filesystem')
            e.fd.seek(512);e.fd.write(stage)
            e.fd.seek(9*512);e.fd.write(kernel.ljust(1280*1024,b'\0'))
            report['kernel_changed']=True
            report['kernel_sha256']=hashlib.sha256(kernel).hexdigest()
            report['stage2_sha256']=hashlib.sha256(stage).hexdigest()
        e.finish();report['preserved_inodes']=len(before)-len(changed)
    finally:e.fd.close()
    bfs4.check(temp);os.replace(temp,image)
    image.with_suffix('.quiet.json').write_text(json.dumps(report,indent=2))
finally:temp.unlink(missing_ok=True)
print('Updated:',image,'; backup:',backup)
