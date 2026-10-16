#!/usr/bin/env python3
"""Exercise repairs of a missing/known/unknown executable on real BFS4 copies."""
import hashlib,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'tools'))
from bfs4_update import Editor
from repair_fifa2000 import GAME,ORIGINAL,ALTERNATE,registry_defaults
iso=root/'media/installers/FIFA2000.iso'
if not iso.is_file():raise SystemExit('This integration test requires the owner-provided FIFA2000.iso')
reg=(r'[Software\\EA SPORTS\\FIFA 2000]'+'\n"Language"="Spanish"\n"Thrash Driver"="softdraw"\n"Other"="keep"\n\n[Unrelated]\n"value"="preserve"\n').encode()
with tempfile.TemporaryDirectory(prefix='bk-fifa-repair-') as directory:
 work=Path(directory);tree=work/'root';(tree/GAME.lstrip('/')).mkdir(parents=True)
 prefix=tree/'WINEGATE/prefix';(prefix/'dosdevices').mkdir(parents=True)
 (prefix/'system.reg').write_bytes(reg);(prefix/'dosdevices/d:').write_bytes(b'/CDROM')
 (tree/GAME.lstrip('/')/'keep.txt').write_bytes(b'installed data must survive')
 image=work/'test.img'
 with image.open('wb') as stream:stream.truncate(32*1024*1024)
 subprocess.run([sys.executable,str(root/'tools/bfs4.py'),'pack',str(image),str(tree),'--size-mib','32'],check=True)
 command=[sys.executable,str(root/'tools/repair_fifa2000.py'),'--image',str(image),'--iso',str(iso),'--use-disc-executable','--renderer','dx6']
 subprocess.run(command,check=True)
 e=Editor(image)
 assert hashlib.sha256(e.contents(GAME+'/fifa2000.exe')).hexdigest()==ALTERNATE
 assert hashlib.sha256(e.contents(GAME+'/fifa2000.exe.safedisc-backup')).hexdigest()==ORIGINAL
 assert e.contents('/WINEGATE/prefix/system.reg.fifa2000-backup')==reg
 updated=e.contents('/WINEGATE/prefix/system.reg')
 assert b'"Thrash Driver"="dx6"' in updated and b'"Language"="Spanish"' in updated and b'"value"="preserve"' in updated
 assert e.contents(GAME+'/keep.txt')==b'installed data must survive';e.fd.close()
 subprocess.run(command,check=True)
 e=Editor(image);assert e.contents('/WINEGATE/prefix/system.reg')==updated
 e.update(GAME+'/fifa2000.exe',b'unknown user executable');e.finish()
 before=hashlib.sha256(image.read_bytes()).hexdigest()
 r=subprocess.run(command,capture_output=True,text=True)
 assert r.returncode and 'unknown installed executable' in r.stderr
 assert hashlib.sha256(image.read_bytes()).hexdigest()==before
 assert not image.with_suffix('.img.fifa-repair').exists()
print('PASS: restore omitted EXE, preserve verified original and settings, select Direct3D explicitly, idempotent, refuse unknown EXE atomically')
