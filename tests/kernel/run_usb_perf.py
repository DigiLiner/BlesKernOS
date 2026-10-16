#!/usr/bin/env python3
"""Compare UHCI USB-root boots and checked native reads with old/new kernels."""
import argparse,json,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
import build_usb_image
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--payload',type=Path,default=ROOT/'build')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--controller',choices=['uhci','ehci'],default='uhci')
p.add_argument('--timeout',type=int,default=300,help='cold USB 1.1 root boots can require more time in TCG')
p.add_argument('--check-monitor',action='store_true',help='verify the Performance tab repaints over USB')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
base=a.out/'part.img';usb=a.out/'test.img';sock=Path('/tmp/bk-usb-perf-'+a.out.name+'.sock');sock.unlink(missing_ok=True)
subprocess.run(['cp','--reflink=auto',str(a.base),str(base)],check=True)
def payload(name):
 candidate=a.payload/name
 return candidate if candidate.exists() else ROOT/'build'/name
with base.open('r+b') as f:
 f.seek(512);f.write(payload('stage2.bin').read_bytes());f.seek(9*512);f.write(payload('kernel.bin').read_bytes())
obj=a.out/'usb_perf.o'
subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-c',str(ROOT/'tests/userland/usb_perf_smoke.c'),'-o',str(obj)],check=True)
data=bytes((i*37+i//251)&255 for i in range(1048576));h=2166136261
for b in data:h=((h^b)*16777619)&0xffffffff
# The guest checksum is an independent check of transport integrity.
assert h==0x7f65ea70,hex(h)
e=Editor(base)
e.update('/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX',payload('processmanager.o' if a.payload!=ROOT/'build' else 'programs/processmanager.o').read_bytes())
e.update('/SYSTEM/PROGRAMS/USBPERF.BEX',obj.read_bytes());e.update('/SYSTEM/PROGRAMS/USBPERF.DAT',data)
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|USB perf|/SYSTEM/PROGRAMS/USBPERF.BEX||USB transport test\n');e.finish()
subprocess.run([sys.executable,str(ROOT/'tools/build_usb_image.py'),str(usb),str(base),str(ROOT/'build/mbr.bin')],check=True);base.unlink()
serial=a.out/'boot.log';serial.unlink(missing_ok=True);start=time.monotonic()
cmd=['qemu-system-i386','-accel','tcg','-cpu','pentium3','-smp','1','-m','128','-M','pc,usb=off',
 '-device',('piix4-usb-uhci,id=usbhost' if a.controller=='uhci' else 'usb-ehci,id=usbhost'),'-drive',f'file={usb},format=raw,if=none,id=usbdisk',
 '-device','usb-storage,bus=usbhost.0,drive=usbdisk,bootindex=1','-snapshot','-vga','std','-display','none',
 '-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown']
(a.out/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
proc=subprocess.Popen(cmd,stderr=(a.out/'qemu.log').open('w'))
try:
 desktop_elapsed=None;deadline=start+a.timeout
 while time.monotonic()<deadline:
  log=serial.read_text(errors='replace') if serial.exists() else ''
  if desktop_elapsed is None and 'DESKTOP RELEASED' in log:desktop_elapsed=time.monotonic()-start
  if '[USBPERF] DONE' in log:break
  if '[PANIC]' in log or 'PAGE FAULT' in log or '[TASK] excepcion' in log:raise RuntimeError('Guest fault')
  if proc.poll() is not None:raise RuntimeError('QEMU exited: '+(a.out/'qemu.log').read_text())
  time.sleep(.2)
 s=socket.socket(socket.AF_UNIX);s.settimeout(10);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
 def qmp(command,args=None):
  f.write((json.dumps({'execute':command,'arguments':args or {}})+'\n').encode())
  while True:
   r=json.loads(f.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 qmp('qmp_capabilities');qmp('screendump',{'filename':str(a.out/'desktop.ppm')})
 monitor_changed=None
 if a.check_monitor:
  def hmp(command):return qmp('human-monitor-command',{'command-line':command})
  # Calibrate PS/2 position, using bounded packets like the desktop tests.
  for _ in range(25):hmp('mouse_move -40 -40');time.sleep(.025)
  position=[0,0]
  def click(x,y):
   while position!=[x,y]:
    dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]))
    hmp(f'mouse_move {dx} {dy}');position[0]+=dx;position[1]+=dy;time.sleep(.03)
   time.sleep(.15);hmp('mouse_button 1');time.sleep(.15)
   hmp('mouse_button 0');time.sleep(.5)
  click(200,38);click(250,72);time.sleep(2)
  qmp('screendump',{'filename':str(a.out/'monitor-a.ppm')});time.sleep(4)
  qmp('screendump',{'filename':str(a.out/'monitor-b.ppm')})
  from PIL import Image,ImageChops
  first=Image.open(a.out/'monitor-a.ppm').convert('RGB');second=Image.open(a.out/'monitor-b.ppm').convert('RGB')
  first.save(a.out/'monitor-a.png');second.save(a.out/'monitor-b.png')
  region=(80,100,700,420);pixels=list(first.crop(region).getdata())
  assert sum(1 for color in pixels if color==(0,0,0))>10000,'Performance graphs were not opened'
  monitor_changed=sum(1 for color in ImageChops.difference(first.crop(region),second.crop(region)).getdata() if color!=(0,0,0))
  assert monitor_changed>20,'Performance graphs did not advance'
 qmp('quit');proc.wait(timeout=10);s.close()
 import re
 log=serial.read_text(errors='replace')
 result={'controller':a.controller,'desktop_wall_seconds':desktop_elapsed,'reads':[{'pass':int(p),'bytes':int(b),'ticks':int(t),'hz':int(hz),'valid':int(v)} for p,b,h,t,hz,v in re.findall(r'USBPERF\] pass=(\d+) bytes=(\d+) hash=(?:0x)?([0-9a-fA-F]+) ticks=(\d+) hz=(\d+) valid=(\d+)',log)],'pass':'[USBPERF] DONE result=PASS' in log,'root_usb':'Montado usb0 OK' in log}
 if a.check_monitor:result['monitor_changed_pixels']=monitor_changed
 (a.out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));assert result['pass'] and result['root_usb'] and len(result['reads'])==2 and all(r['valid'] for r in result['reads']),'USB test did not pass; see boot.log'
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);usb.unlink(missing_ok=True)
