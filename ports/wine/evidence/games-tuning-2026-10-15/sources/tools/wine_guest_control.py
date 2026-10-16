#!/usr/bin/env python3
"""QMP control and capture of the real guest framebuffer (also EGL-headless)."""
import argparse,json,re,struct,subprocess,sys,time
from pathlib import Path
from PIL import Image
from run_qemu_graphics_validation import qmp
p=argparse.ArgumentParser(description=__doc__);p.add_argument('out',type=Path)
p.add_argument('--windows',action='store_true');p.add_argument('--click',nargs=2,type=int);p.add_argument('--capture');p.add_argument('--key');p.add_argument('--hmp');p.add_argument('--quit',action='store_true')
a=p.parse_args();out=a.out.resolve();sock=Path(json.loads((out/'command.json').read_text())['qmp'])
if a.key:
 for down,codes in [(True,a.key.split('-')),(False,list(reversed(a.key.split('-'))))]:
  qmp(sock,'input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':c}}} for c in codes]})
  time.sleep(.7)
if a.hmp:print(qmp(sock,'human-monitor-command',{'command-line':a.hmp}))
if a.capture:
 symbols=subprocess.check_output(['nm',str(next(out.glob('stage2-kernel/kernel-test.elf')))],text=True)
 address=int(re.search(r'^([0-9a-f]+) b g_gfx$',symbols,re.M)[1],16)
 def memory(addr,size):
  path=out/'capture.bin';qmp(sock,'pmemsave',{'val':addr,'size':size,'filename':str(path)});return path.read_bytes()
 qmp(sock,'stop')
 try:
  mode,fb,w,h,pitch,bpp=struct.unpack_from('<IIHHHB',memory(address,16))
  if not (w and h and bpp==32 and fb):raise RuntimeError(f'unsupported capture {mode,fb,w,h,pitch,bpp}')
  raw=memory(fb,pitch*h);Image.frombytes('RGB',(w,h),raw,'raw','BGRX',pitch).save(out/(a.capture+'.png'))
 finally:qmp(sock,'cont')
 print(out/(a.capture+'.png'))
if a.click:
 root=Path(__file__).resolve().parents[1]
 source=out/'mouse-offsets.c';binary=out/'mouse-offsets'
 source.write_text('#include "kernel/include/task.h"\n#include "gui/gui.h"\nextern int printf(const char *,...);\nint main(void){printf("%u %u",(unsigned)__builtin_offsetof(gui_desktop_t,mouse_x),(unsigned)__builtin_offsetof(gui_desktop_t,mouse_y));}\n')
 subprocess.run(['gcc','-m32','-I'+str(root),str(source),'-o',str(binary)],check=True)
 xoff,yoff=map(int,subprocess.check_output([str(binary)],text=True).split())
 symbols=subprocess.check_output(['nm',str(out/'stage2-kernel/kernel-test.elf')],text=True)
 address=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
 def read(addr,size):
  path=out/'mouse.bin';qmp(sock,'pmemsave',{'val':addr,'size':size,'filename':str(path)});return path.read_bytes()
 desktop=struct.unpack('<I',read(address,4))[0]
 for attempt in range(100):
  x=struct.unpack('<i',read(desktop+xoff,4))[0];y=struct.unpack('<i',read(desktop+yoff,4))[0]
  dx=a.click[0]-x;dy=a.click[1]-y
  if abs(dx)<=2 and abs(dy)<=2:break
  qmp(sock,'human-monitor-command',{'command-line':'mouse_move %d %d'%(max(-40,min(40,dx)),max(-40,min(40,dy)))})
  time.sleep(.08)
 else:raise RuntimeError('mouse did not reach target')
 for mask in [1,0]:
  qmp(sock,'human-monitor-command',{'command-line':f'mouse_button {mask}'});time.sleep(.7)
if a.windows:
 root=Path(__file__).resolve().parents[1];source=out/'window-offsets.c';binary=out/'window-offsets'
 source.write_text('#include "kernel/include/task.h"\n#include "gui/gui.h"\nextern int printf(const char *,...);\n#define O(t,m) ((unsigned)__builtin_offsetof(t,m))\nint main(void){printf("%u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_window_t,next),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,stream_width),O(gui_window_t,stream_height),O(gui_window_t,stream_pixels),sizeof(gui_window_t));}\n')
 subprocess.run(['gcc','-m32','-I'+str(root),str(source),'-o',str(binary)],check=True)
 first,nxt,bounds,title,sw,sh,sp,size=map(int,subprocess.check_output([str(binary)],text=True).split())
 symbols=subprocess.check_output(['nm',str(out/'stage2-kernel/kernel-test.elf')],text=True);address=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
 def read(addr,size):
  path=out/'window.bin';qmp(sock,'pmemsave',{'val':addr,'size':size,'filename':str(path)});return path.read_bytes()
 desktop=struct.unpack('<I',read(address,4))[0];ptr=struct.unpack('<I',read(desktop+first,4))[0];seen=set();windows=[]
 while ptr and ptr not in seen and len(seen)<64:
  seen.add(ptr);w=read(ptr,size);windows.append({'handle':ptr,'title':w[title:title+48].split(b'\0')[0].decode(errors='replace'),'bounds':struct.unpack_from('<iiii',w,bounds),'stream':(struct.unpack_from('<H',w,sw)[0],struct.unpack_from('<H',w,sh)[0]),'pixels':struct.unpack_from('<I',w,sp)[0]});ptr=struct.unpack_from('<I',w,nxt)[0]
 print(json.dumps(windows,indent=2));(out/'windows.json').write_text(json.dumps(windows,indent=2)+'\n')
if a.quit:qmp(sock,'quit')
