#!/usr/bin/env python3
"""Capture all control panel tabs in an isolated QEMU image."""
import socket,json,time,subprocess,sys,argparse
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'));from bfs4_update import Editor
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-controls.img');args=parser.parse_args()
out=ROOT/'tests/desktop/evidence/control-panels-2026-10-09';out.mkdir(parents=True,exist_ok=True);image=ROOT/'build/control-panels-test.img'
subprocess.run(['cp','--reflink=auto',str(args.base),str(image)],check=True)
e=Editor(image);e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'3|0|2|2|1|1|180000|Wine|/WINEGATE/bin/wine-launch.ELF||Windows\n4|1|1|10|0|1|30000|Control|/SYSTEM/CONTROL/CONTROL.BEX||UI test\n');e.update('/SYSTEM/USER/CONFIG/MOUSE.INI',b'sensitivity=3\r\ntrail=0\r\nswap=0\r\ndouble_click=500\r\nscheme=0\r\n');e.update('/SYSTEM/USER/CONFIG/KEYBD.INI',b'layout=US\r\ndelay=500\r\ninterval=60\r\nblink=500\r\n');e.update('/SYSTEM/USER/CONFIG/INTERNET.INI',b'images=1\r\ncss=1\r\ncookies=1\r\ntimeout=10000\r\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
sock=Path('/tmp/bk-panels-visual.sock');sock.unlink(missing_ok=True);serial=out/'boot.log'
proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-m','256','-smp','2','-drive',f'file={image},format=raw,if=ide','-vga','std','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-device','pci-serial','-netdev','user,id=n0','-device','rtl8139,netdev=n0'],stderr=(out/'qemu.log').open('w'))
try:
 for _ in range(100):
  if sock.exists():break
  time.sleep(.1)
 client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(15);client.connect(str(sock));f=client.makefile('rwb');f.readline()
 def cmd(name,args={}):
  f.write((json.dumps({'execute':name,'arguments':args})+'\n').encode());f.flush()
  while True:
   r=json.loads(f.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 cmd('qmp_capabilities')
 for _ in range(180):
  if serial.exists() and 'DESKTOP RELEASED' in serial.read_text(errors='replace'):break
  time.sleep(.5)
 else:raise TimeoutError('Desktop boot')
 time.sleep(4)
 def shot(name):
  time.sleep(.7) # Let the Ring 3 content frame commit before capture.
  p=out/(name+'.ppm');cmd('screendump',{'filename':str(p)});im=Image.open(p);im.save(p.with_suffix('.png'));return im
 def hmp(text):return cmd('human-monitor-command',{'command-line':text})
 for _ in range(25):hmp('mouse_move -40 -40');time.sleep(.025)
 position=[0,0]
 def move(x,y):
  while position!=[x,y]:
   dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]));hmp(f'mouse_move {dx} {dy}');position[0]+=dx;position[1]+=dy;time.sleep(.03)
  time.sleep(.15)
 def click(x,y,button=1):
  move(x,y);hmp(f'mouse_button {button}');time.sleep(.10);hmp('mouse_button 0');time.sleep(.12)
 shot('control-panel')
 # Coordinates are for the fixed 800x600 desktop and authored window bounds.
 click(292,292);hmp('sendkey ret');time.sleep(2);shot('keyboard-speed');print('keyboard-speed',flush=True)
 # Drag the delay to its short end, then verify it persisted after shutdown.
 move(342,167);hmp('mouse_button 1');move(532,167);hmp('mouse_button 0');time.sleep(1);shot('keyboard-slider')
 click(244,83);shot('keyboard-language');click(314,83);shot('keyboard-general');click(572,52)
 click(580,292);hmp('sendkey ret');time.sleep(2);shot('internet-general');print('internet',flush=True)
 click(238,83);shot('internet-content');click(148,154);click(470,459);time.sleep(1);shot('internet-checkbox')
 click(334,83);shot('internet-connection');click(436,83);shot('internet-programs');click(636,52)
 click(580,200);hmp('sendkey ret');time.sleep(2);shot('mouse-pointers');print('mouse',flush=True)
 click(237,108);shot('mouse-motion');click(368,196);time.sleep(.5);shot('mouse-checkbox');click(324,108);shot('mouse-buttons');click(396,108);shot('mouse-general');click(570,58)
 click(340,383);hmp('sendkey ret');time.sleep(2);shot('modem-devices');print('modem',flush=True)
 click(254,83);shot('modem-connection');click(452,147);shot('modem-dropdown');click(284,180);shot('modem-selection');click(540,481)
 click(340,83);shot('modem-dial');click(444,83);shot('modem-diagnostics')
 cmd('quit');proc.wait(timeout=10)
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True)

# Reopen the isolated image read-only after QEMU closes and check real persistence.
e=Editor(image)
try:
 keyboard=e.contents('/SYSTEM/USER/CONFIG/KEYBD.INI').decode()
 internet=e.contents('/SYSTEM/USER/CONFIG/INTERNET.INI').decode()
 mouse=e.contents('/SYSTEM/USER/CONFIG/MOUSE.INI').decode()
 assert 'delay=250' in keyboard,keyboard
 assert 'images=0' in internet,internet
 assert 'trail=1' in mouse,mouse
 modem=e.contents('/SYSTEM/USER/CONFIG/MODEM.INI').decode()
 assert 'port=COM2' in modem and 'baud=57600' in modem,modem
 (out/'widgets-result.json').write_text(json.dumps({'status':'pass','memory_mib':256,'cpus':2,'keyboard_delay_ms':250,'internet_images':False,'mouse_trail':True,'modem_port':'COM2','modem_baud':57600},indent=2)+'\n')
 print('PASS QEMU controls: slider, checkboxes and dropdown changes persisted',flush=True)
finally:e.fd.close()
