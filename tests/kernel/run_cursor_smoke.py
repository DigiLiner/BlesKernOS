#!/usr/bin/env python3
"""Boot an isolated 32 MiB VMware guest and observe stationary animated cursors."""
import hashlib,json,re,socket,subprocess,sys,time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
out=ROOT/'tests/kernel/evidence/retrosmart-2026-10-10';out.mkdir(parents=True,exist_ok=True)
image=out/'test.img';sock=Path('/tmp/bk-retrosmart-test.sock');sock.unlink(missing_ok=True)
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-ata-user.img'),str(image)],check=True)
obj=out/'cursor_smoke.o'
subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-c',str(ROOT/'tests/userland/cursor_smoke.c'),'-o',str(obj)],check=True)
with image.open('r+b') as f:
 f.seek(512);f.write((ROOT/'build/stage2.bin').read_bytes())
 f.seek(9*512);f.write((ROOT/'build/kernel.bin').read_bytes())
e=Editor(image);e.mkdir('/SYSTEM/CURSORS')
for source in sorted((ROOT/'assets/cursors').iterdir()):e.update('/SYSTEM/CURSORS/'+source.name,source.read_bytes())
e.update('/SYSTEM/PROGRAMS/RAMTEST.BEX',obj.read_bytes())
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Cursor test|/SYSTEM/PROGRAMS/RAMTEST.BEX||Cursor test\n')
e.update('/SYSTEM/USER/CONFIG/MOUSE.INI',b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n');e.finish()
serial=out/'boot.log';serial.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-smp','2','-m','32','-drive',f'file={image},format=raw,if=ide','-snapshot','-vga','vmware','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown','-netdev','user,id=net0','-device','rtl8139,netdev=net0','-device','AC97'],stderr=(out/'qemu.log').open('w'))
try:
 deadline=time.monotonic()+120
 while not sock.exists():
  if time.monotonic()>deadline:raise TimeoutError('QMP')
  time.sleep(.1)
 client=socket.socket(socket.AF_UNIX);client.settimeout(10);client.connect(str(sock));f=client.makefile('rwb',buffering=0);f.readline()
 def cmd(name,args=None):
  f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
  while True:
   result=json.loads(f.readline())
   if 'error' in result:raise RuntimeError(result)
   if 'return' in result:return result['return']
 cmd('qmp_capabilities')
 def await_stage(stage):
  while time.monotonic()<deadline:
   log=serial.read_text(errors='replace') if serial.exists() else ''
   if '[PANIC]' in log or '[TASK] excepcion' in log or 'simbolo no resuelto' in log:raise RuntimeError('Guest failed')
   m=re.search(r'\[CURSORTEST\] '+stage+r' x=(\d+) y=(\d+)',log)
   if m:return tuple(map(int,m.groups()))
   time.sleep(.05)
  raise TimeoutError(stage)
 def shot(name):
  p=out/(name+'.ppm');cmd('screendump',{'filename':str(p)})
  im=Image.open(p).convert('RGB');im.save(p.with_suffix('.png'));p.unlink();return im
 result={'ram_mib':32,'cpus':2,'vga':'vmware','stationary_animation':{}}
 await_stage('ARROW');shot('arrow')
 for stage in ['WAIT','PROGRESS']:
  x,y=await_stage(stage);hashes=[];frames=[]
  time.sleep(.2)
  for i in range(15):
   im=shot(stage.lower()+f'-{i:02}');crop=im.crop((max(0,x-32),max(0,y-32),x+32,y+32))
   hashes.append(hashlib.sha256(crop.tobytes()).hexdigest());frames.append(crop)
   time.sleep(.135 if stage=='WAIT' else .105)
  unique=len(set(hashes));assert unique>=3,(stage,unique)
  frames[0].save(out/(stage.lower()+'.gif'),save_all=True,append_images=frames[1:],duration=135 if stage=='WAIT' else 105,loop=0)
  result['stationary_animation'][stage]={'distinct_frames':unique,'x':x,'y':y}
 while '[CURSORTEST] DONE' not in serial.read_text(errors='replace'):
  if time.monotonic()>deadline:raise TimeoutError('DONE')
  time.sleep(.1)
 time.sleep(.4);shot('arrow-restored');result['status']='PASS'
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 cmd('quit');proc.wait(timeout=10)
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
