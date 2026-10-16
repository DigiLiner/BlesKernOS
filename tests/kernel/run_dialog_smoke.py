#!/usr/bin/env python3
"""File selection, About menu and callback recovery through the graphical server."""
import argparse,json,re,socket,struct,subprocess,sys,time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--out',type=Path,default=ROOT/'tests/kernel/evidence/phase4-dialogs')
p.add_argument('--ram',type=int,default=64);p.add_argument('--smp',type=int,default=2)
p.add_argument('--vga',default='std');a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['make','build/stage2.bin','build/system/services/vision.o','build/system/drivers/PS2MOUSE.DVR'],cwd=ROOT,check=True)
obj=out/'wine_gui_smoke.o'
subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-c',str(ROOT/'tests/userland/dialog_smoke.c'),'-o',str(obj)],check=True)
# Compile structure offsets from the actual production headers. Observation is
# through QMP physical RAM in the disposable VM, not a new guest debug syscall.
offsets=out/'offsets.c';offsets.write_text('''#include "gui/gui.h"
extern int printf(const char *,...);
#define O(type,member) ((unsigned)__builtin_offsetof(type,member))
int main(void){printf("%u %u %u %u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_desktop_t,mouse_x),O(gui_desktop_t,mouse_y),sizeof(gui_window_t),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,next),O(gui_window_t,visible),O(gui_window_t,listed),O(gui_window_t,owner_pid),O(gui_window_t,controller_pid));}
''')
subprocess.run(['gcc','-m32','-I'+str(ROOT),str(offsets),'-o',str(out/'offsets')],check=True)
keys=['first','mouse_x','mouse_y','window_size','bounds','title','next','visible','listed','owner','controller']
layout=dict(zip(keys,map(int,subprocess.check_output([str(out/'offsets')],text=True).split())))
symbols=subprocess.check_output(['nm',str(ROOT/'build/kernel.elf')],text=True)
desktop_address=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
image=out/'test.img';subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
with image.open('r+b') as f:
 f.seek(512);f.write((ROOT/'build/stage2.bin').read_bytes())
 kernel=(ROOT/'build/kernel.bin').read_bytes();f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'))
e=Editor(image);e.update('/SYSTEM/DRIVERS/PS2MOUSE.DVR',(ROOT/'build/system/drivers/PS2MOUSE.DVR').read_bytes());e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/system/services/vision.o').read_bytes());e.update('/SYSTEM/PROGRAMS/DIALOGTEST.BEX',obj.read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Dialogs test|/SYSTEM/PROGRAMS/DIALOGTEST.BEX||Disposable Notepad test\n');e.finish()
sock=Path('/tmp/bk-wine-ui-'+str(__import__('os').getpid())+'.sock');serial=out/'boot.log';serial.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-smp',str(a.smp),'-m',str(a.ram),'-drive',f'file={image},format=raw,if=ide','-snapshot','-vga',a.vga,'-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-nodefaults','-no-reboot','-no-shutdown'],stderr=(out/'qemu.log').open('w'))
try:
 deadline=time.monotonic()+600
 while not sock.exists():
  if proc.poll() is not None:raise RuntimeError((out/'qemu.log').read_text())
  if time.monotonic()>deadline:raise TimeoutError('QMP startup')
  time.sleep(.1)
 s=socket.socket(socket.AF_UNIX);s.settimeout(15);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
 def cmd(name,arguments=None):
  f.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
  while True:
   r=json.loads(f.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 cmd('qmp_capabilities')
 def log():
  text=serial.read_text(errors='replace')
  if '[PANIC]' in text or '[TASK] excepcion' in text or 'sin memoria' in text or 'simbolo no resuelto' in text:raise RuntimeError('Guest fault; see '+str(serial))
  return text
 def wait_marker(marker):
  while time.monotonic()<deadline:
   if marker in log():return
   if proc.poll() is not None:raise RuntimeError('QEMU stopped')
   time.sleep(.25)
  raise TimeoutError(marker)
 def memory(address,size):
  target=out/'memory.bin';cmd('pmemsave',{'val':address,'size':size,'filename':str(target)})
  return target.read_bytes()
 def windows():
  cmd('stop')
  try:
   desktop=struct.unpack('<I',memory(desktop_address,4))[0]
   data=memory(desktop,layout['mouse_y']+4)
   pointer=struct.unpack_from('<I',data,layout['first'])[0];result=[];seen=set()
   while pointer and pointer not in seen and len(seen)<128:
    if pointer<0x400000 or pointer+layout['window_size']>a.ram*1024*1024:raise RuntimeError('Bad guest window address')
    seen.add(pointer);w=memory(pointer,layout['window_size'])
    result.append({'handle':pointer,'bounds':struct.unpack_from('<iiii',w,layout['bounds']),'title':w[layout['title']:layout['title']+48].split(b'\0')[0].decode(errors='replace'),'visible':bool(w[layout['visible']]),'listed':bool(w[layout['listed']]),'owner':struct.unpack_from('<I',w,layout['owner'])[0],'controller':struct.unpack_from('<I',w,layout['controller'])[0]})
    pointer=struct.unpack_from('<I',w,layout['next'])[0]
   return result,(struct.unpack_from('<i',data,layout['mouse_x'])[0],struct.unpack_from('<i',data,layout['mouse_y'])[0])
  finally:cmd('cont')
 def find_window(title):
  while time.monotonic()<deadline:
   log();items,_=windows()
   for w in items:
    if title.lower() in w['title'].lower() and w['visible'] and w['listed']:return w
   time.sleep(.5)
  raise TimeoutError('Window: '+title)
 def move(x,y):
  for attempt in range(100):
   _,position=windows()
   if abs(position[0]-x)<=2 and abs(position[1]-y)<=2:return
   dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]))
   cmd('human-monitor-command',{'command-line':f'mouse_move {dx} {dy}'})
   time.sleep(.3)
  raise TimeoutError('Mouse did not reach target '+str((x,y,position)))
 def click(x,y):
  move(x,y);cmd('human-monitor-command',{'command-line':'mouse_button 1'});time.sleep(.1);cmd('human-monitor-command',{'command-line':'mouse_button 0'});time.sleep(.8)
 def key(text):
  cmd('human-monitor-command',{'command-line':'sendkey '+text});time.sleep(.12)
 def shot(name):
  file=out/(name+'.ppm');cmd('screendump',{'filename':str(file)});Image.open(file).save(file.with_suffix('.png'))
 wait_marker('[DIALOGTEST] attached=1 opened=1')
 selector=find_window('Selector Ring3');x,y,w,h=selector['bounds'];shot('selector');click(x+w-14,y+12)
 wait_marker('[DIALOGTEST] cancellation callback=1');about=find_window('About Ring3');x,y,w,h=about['bounds'];shot('about');click(x+w-14,y+12)
 monitor=find_window('Dialog monitor');x,y,w,h=monitor['bounds'];click(x+30,y+80);key('f11');wait_marker('[DIALOGTEST] replacement ready')
 monitor=find_window('Dialog monitor');x,y,w,h=monitor['bounds'];click(x+20,y+34);time.sleep(.5);click(x+30,y+56)
 restored=find_window('About Ring3');x,y,w,h=restored['bounds'];shot('about-restored');click(x+w-14,y+12)
 monitor=find_window('Dialog monitor');x,y,w,h=monitor['bounds'];click(x+30,y+80);key('f12');wait_marker('[DIALOGTEST] DONE result=PASS')
 result={'result':'PASS','selector_cancelled':True,'about_before_after_restart':True,'native_callback':True,'compositor_cpl3':'compositor y ventanas CPL=3' in log()}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
 cmd('quit');proc.wait(timeout=10);s.close()
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True);(out/'memory.bin').unlink(missing_ok=True)
