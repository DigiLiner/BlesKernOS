#!/usr/bin/env python3
"""Boot isolated snapshots, check savers animate and close with Escape."""
import argparse, json, socket, subprocess, sys, time
from pathlib import Path
from PIL import Image, ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,default=ROOT/'tests/kernel/evidence/phase4-savers');parser.add_argument('--smp',type=int,default=2);parser.add_argument('--cpu',default='host');parser.add_argument('--savers',nargs='+',choices=['SSBALLS','SSPIPES','SSLOGO'],default=['SSBALLS','SSPIPES']);args=parser.parse_args();out=args.out;out.mkdir(parents=True,exist_ok=True)
subprocess.run(['make','build/stage2.bin','build/system/services/vision.o','build/system/drivers/PS2MOUSE.DVR','build/system/screensavers/ss_balls.o','build/system/screensavers/ss_pipes.o','build/system/screensavers/ss_logo.o'],cwd=ROOT,check=True)
stage2=(ROOT/'build/stage2.bin').read_bytes();kernel=(ROOT/'build/kernel.bin').read_bytes();vision=(ROOT/'build/system/services/vision.o').read_bytes()
for name in args.savers:
 image=out/(name+'.img');sock=Path('/tmp/bk-saver-'+name+'.sock');sock.unlink(missing_ok=True)
 subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-ata-user.img'),str(image)],check=True)
 with image.open('r+b') as disk:
  disk.seek(512);disk.write(stage2)
  disk.seek(9*512);disk.write(kernel.ljust(1280*1024,b'\0'))
 e=Editor(image);e.update('/SYSTEM/DRIVERS/PS2MOUSE.DVR',(ROOT/'build/system/drivers/PS2MOUSE.DVR').read_bytes());e.update('/SYSTEM/SERVICES/VISION.BEX',vision)
 source={'SSBALLS':'ss_balls.o','SSPIPES':'ss_pipes.o','SSLOGO':'ss_logo.o'}[name]
 e.update(f'/SYSTEM/SCREENS/{name}.SCV',(ROOT/'build/system/screensavers'/source).read_bytes())
 e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',f'4|1|1|10|0|1|3000|Saver test|/SYSTEM/SCREENS/{name}.SCV||Animation test\n'.encode());e.finish()
 log=out/(name+'.log');log.unlink(missing_ok=True)
 proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu',args.cpu,'-smp',str(args.smp),'-m','128','-drive',f'file={image},format=raw,if=ide','-snapshot','-vga','vmware','-display','none','-serial',f'file:{log}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown'],stderr=(out/(name+'-qemu.log')).open('w'))
 try:
  deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   if log.exists() and 'DESKTOP RELEASED' in log.read_text(errors='replace'):break
   if proc.poll() is not None:raise RuntimeError('QEMU exited')
   time.sleep(.2)
  assert 'DESKTOP RELEASED' in log.read_text(errors='replace')
  s=socket.socket(socket.AF_UNIX);s.settimeout(15);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
  def cmd(command,args=None):
   f.write((json.dumps({'execute':command,'arguments':args or {}})+'\n').encode())
   while True:
    r=json.loads(f.readline())
    if 'error' in r:raise RuntimeError(r)
    if 'return' in r:return r['return']
  cmd('qmp_capabilities');time.sleep(30)
  shots=[];changed=0;animation_deadline=time.monotonic()+30
  for i in range(16):
   ppm=out/f'{name}-{i}.ppm';cmd('screendump',{'filename':str(ppm)})
   im=Image.open(ppm).convert('RGB');im.save(ppm.with_suffix('.png'));shots.append(im)
   if len(shots)>1:
    changed=sum(p!=(0,0,0) for p in ImageChops.difference(shots[0],shots[-1]).getdata())
    if changed>300:break
   if time.monotonic()>=animation_deadline:break
   time.sleep(2)
  black=sum(max(p)<20 for p in shots[0].getdata())
  scene=sum(max(p)<32 or max(p)-min(p)>32 for p in shots[0].getdata())
  assert scene>shots[0].width*shots[0].height*3//4, 'Saver did not cover the desktop'
  assert changed>300, 'Saver did not animate'
  cmd('send-key',{'keys':[{'type':'qcode','data':'esc'}]});time.sleep(3)
  ppm=out/f'{name}-closed.ppm';cmd('screendump',{'filename':str(ppm)})
  closed=Image.open(ppm).convert('RGB');closed.save(ppm.with_suffix('.png'))
  black_closed=sum(max(p)<20 for p in closed.getdata());assert black_closed<black//2,'Saver did not close'
  text=log.read_text(errors='replace');assert '[TASK] excepcion' not in text and '[PANIC]' not in text
  result={'saver':name,'cpu':args.cpu,'smp':args.smp,'animated_pixels_changed':changed,'black_pixels':black,'closed_black_pixels':black_closed,'passed':True}
  (out/(name+'-result.json')).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
  cmd('quit');proc.wait(timeout=10);s.close()
 finally:
  if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
  sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
