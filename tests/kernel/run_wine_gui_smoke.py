#!/usr/bin/env python3
"""Notepad/TinyX input and clipboard through a real restarted Ring-3 compositor."""
import argparse,json,re,socket,struct,subprocess,sys,time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--out',type=Path,default=ROOT/'tests/kernel/evidence/phase4-wine-ui')
p.add_argument('--ram',type=int,default=512);p.add_argument('--smp',type=int,default=2)
p.add_argument('--wine-debug',default='err+all');p.add_argument('--vga',default='std')
p.add_argument('--disk-bus',choices=['ide','ahci'],default='ide',help='isolate storage transport while keeping the same disk contents')
p.add_argument('--kernel-dir',type=Path,help='use saved kernel.bin/kernel.elf/stage2.bin for reproducible comparisons')
p.add_argument('--tinyx',type=Path,help='install this TinyX only in the disposable image')
p.add_argument('--latency-samples',type=int,default=0,help='measure key to displayed glyph before the functional checks (1-32)')
p.add_argument('--latency-only',action='store_true',help='finish after the latency measurements')
p.add_argument('--launch-failure',action='store_true',help='launch a missing EXE and verify that the busy cursor clears')
p.add_argument('--startup-probe',action='store_true',help='verify shared busy cursor until the first Wine window')
p.add_argument('--startup-only',action='store_true',help='record first Wine window and exit without typing or clipboard checks')
p.add_argument('--load-latency',action='store_true',help='measure F7 to native window repaint while Wine starts')
p.add_argument('--load-probe',action='store_true',help='capture blocked task/lock state after the first native repaint timeout; diagnostic only')
p.add_argument('--profile-w3',action='store_true',help='read aggregate counters from an isolated W3 kernel')
p.add_argument('--input-probe',action='store_true',help='also time native queue routing and TinyX consumption through QMP')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
if not 0<=a.latency_samples<=32:p.error('--latency-samples must be between 0 and 32')
if a.latency_only and not a.latency_samples:p.error('--latency-only requires --latency-samples')
if a.input_probe and not a.latency_samples:p.error('--input-probe requires --latency-samples')
if a.launch_failure and not a.startup_probe:p.error('--launch-failure requires --startup-probe')
if a.startup_only and (a.latency_samples or a.launch_failure):p.error('--startup-only cannot measure typing or launch failure')
if a.load_latency and (a.startup_probe or a.launch_failure):p.error('--load-latency must run separately from busy cursor checks')
if a.load_probe and not a.load_latency:p.error('--load-probe requires --load-latency')
subprocess.run(['make',*([] if a.kernel_dir else ['build/stage2.bin']),'build/system/services/vision.o','build/system/drivers/PS2MOUSE.DVR'],cwd=ROOT,check=True)
kernel_dir=a.kernel_dir.resolve() if a.kernel_dir else ROOT/'build'
obj=out/'wine_gui_smoke.o'
fixture=ROOT/'tests/userland/wine_gui_smoke.c'
if a.launch_failure:
 fixture=out/'wine_gui_smoke.c'
 fixture.write_text((ROOT/'tests/userland/wine_gui_smoke.c').read_text().replace('/SYSTEM/WINE/WINDOWS/SYSTEM32/NOTEPAD.EXE','/WINEGATE/no-such-startup-test.exe'))
subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-DWINE_UI_LOAD_PROBE='+str(int(a.load_latency)),'-DWINE_UI_QUIET='+str(int(bool(a.latency_samples or a.startup_probe or a.startup_only or a.load_latency))),'-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-iquote',str(ROOT/'tests/userland'),'-c',str(fixture),'-o',str(obj)],check=True)
# Compile structure offsets from the actual production headers. Observation is
# through QMP physical RAM in the disposable VM, not a new guest debug syscall.
offsets=out/'offsets.c';offsets.write_text('''#include "gui/gui.h"
extern int printf(const char *,...);
#define O(type,member) ((unsigned)__builtin_offsetof(type,member))
int main(void){printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_desktop_t,mouse_x),O(gui_desktop_t,mouse_y),sizeof(gui_window_t),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,next),O(gui_window_t,visible),O(gui_window_t,listed),O(gui_window_t,owner_pid),O(gui_window_t,controller_pid),O(gui_window_t,stream_pixels),O(gui_window_t,stream_width),GUI_BORDER_SIZE,GUI_TITLEBAR_HEIGHT,O(gui_desktop_t,cursor_busy_count),O(gui_desktop_t,focused_window));}
''')
subprocess.run(['gcc','-m32','-I'+str(ROOT),str(offsets),'-o',str(out/'offsets')],check=True)
keys=['first','mouse_x','mouse_y','window_size','bounds','title','next','visible','listed','owner','controller','stream','stream_width','border','title_height','busy_count','focused_window']
layout=dict(zip(keys,map(int,subprocess.check_output([str(out/'offsets')],text=True).split())))
symbols=subprocess.check_output(['nm',str(kernel_dir/'kernel.elf')],text=True)
(out/'loaded-kernel.nm').write_text(symbols)
__import__('shutil').copyfile(kernel_dir/'kernel.elf',out/'loaded-kernel.elf')
desktop_address=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
# Diagnostic-only build changes logging, retaining the production launcher path.
launcher_source=out/'wine-launch.c'
launcher_source.write_text((ROOT/'ports/tinyx/wine-launch.c').read_text().replace('diagnostics = getenv("BLES_WINE_DIAGNOSTICS")','diagnostics = 1 || getenv("BLES_WINE_DIAGNOSTICS")').replace('WINEDEBUG=-all','WINEDEBUG='+a.wine_debug))
launcher=out/'wine-launch-test.ELF'
subprocess.run(['gcc','-m32','-O2','-no-pie','-I'+str(ROOT/'ports/tinyx'),'-Wl,--disable-new-dtags','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,-rpath,/WINEGATE/lib',str(launcher_source),'-o',str(launcher),'-L/usr/lib/i386-linux-gnu','-l:libX11.so.6','-l:libXfixes.so.3'],check=True)
image=out/'test.img';subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
with image.open('r+b') as f:
 f.seek(512);f.write((kernel_dir/'stage2.bin').read_bytes())
 kernel=(kernel_dir/'kernel.bin').read_bytes();f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'))
e=Editor(image);e.update('/SYSTEM/DRIVERS/PS2MOUSE.DVR',(ROOT/'build/system/drivers/PS2MOUSE.DVR').read_bytes());e.update('/WINEGATE/bin/wine-phase4.ELF',launcher.read_bytes());e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/system/services/vision.o').read_bytes());e.update('/SYSTEM/PROGRAMS/WINEUITEST.BEX',obj.read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Wine UI test|/SYSTEM/PROGRAMS/WINEUITEST.BEX||Disposable Notepad test\n');e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\ntimeout=3600\npath=/SYSTEM/SCREENS/SSPIPES.SCV\n');e.finish()
if a.tinyx:
 e=Editor(image);e.update('/WINEGATE/bin/XFBDEV',a.tinyx.read_bytes());e.finish()
sock=Path('/tmp/bk-wine-ui-'+str(__import__('os').getpid())+'.sock');serial=out/'boot.log';serial.unlink(missing_ok=True)
disk_args=['-drive',f'file={image},format=raw,if=ide'] if a.disk_bus=='ide' else ['-drive',f'file={image},format=raw,if=none,id=osdisk','-device','ich9-ahci,id=ahci','-device','ide-hd,drive=osdisk,bus=ahci.0']
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-smp',str(a.smp),'-m',str(a.ram),*disk_args,'-snapshot','-vga',a.vga,'-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-nodefaults','-d','int,cpu_reset,guest_errors','-D',str(out/'cpu-debug.log'),'-no-reboot','-no-shutdown'],stderr=(out/'qemu.log').open('w'))
try:
 deadline=time.monotonic()+900
 while not sock.exists():
  if proc.poll() is not None:raise RuntimeError((out/'qemu.log').read_text())
  if time.monotonic()>deadline:raise TimeoutError('QMP startup')
  time.sleep(.1)
 s=socket.socket(socket.AF_UNIX);s.settimeout(15);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
 def cmd(name,arguments=None):
  f.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
  while True:
   r=json.loads(f.readline())
   if 'event' in r:
    with (out/'qmp-events.jsonl').open('a') as events:events.write(json.dumps(r)+'\n')
   if 'error' in r:
    if name=='cont':
     registers=[]
     for cpu in range(a.smp):
      registers.append(cmd('human-monitor-command',{'command-line':'info registers','cpu-index':cpu}))
     (out/'fault-registers.txt').write_text('\n'.join(registers))
    raise RuntimeError(r)
   if 'return' in r:return r['return']
 cmd('qmp_capabilities')
 def log():
  text=serial.read_text(errors='replace')
  if '[WINEUITEST] DONE result=FAIL' in text:raise RuntimeError('Wine UI fixture failed; see '+str(serial))
  if '[PANIC]' in text or '[TASK] excepcion' in text or 'sin memoria' in text or 'simbolo no resuelto' in text:raise RuntimeError('Guest fault; see '+str(serial))
  if not a.launch_failure and '[WINE:STAGE] TARGET exited raw=' in text and '[WINEUITEST] DONE result=PASS' not in text:raise RuntimeError('Wine target exited before completing UI validation; see '+str(serial))
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
   (out/'windows.json').write_text(json.dumps(result,indent=2)+'\n')
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
  # Explicit releases avoid HMP's guest-timer release overlapping the next
  # shortcut while Wine is busy painting.
  codes=text.split('-')
  for down,ordered in [(True,codes),(False,list(reversed(codes)))]:
   cmd('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}} for code in ordered]})
   time.sleep(.1)
  time.sleep(.5)
 def shot(name):
  file=out/(name+'.ppm');cmd('screendump',{'filename':str(file)});im=Image.open(file).convert('RGB');im.save(file.with_suffix('.png'));return im
 def text_mask(im,bounds):
  x,y,w,h=bounds;line=im.crop((x+9,y+49,x+9+min(w-30,450),y+65)).convert('L')
  mask=line.point(lambda p:255 if p<80 else 0)
  # Ignore the blinking insertion caret, which is a separate trailing column.
  for xx in range(mask.width):
   if sum(mask.getpixel((xx,yy))!=0 for yy in range(mask.height))>=12:
    for yy in range(mask.height):mask.putpixel((xx,yy),0)
  return mask
 def same_pixels(a,b):return __import__('PIL.ImageChops',fromlist=['difference']).difference(a,b).getbbox() is None
 def wait_text(name,bounds,reference,duplicate):
  until=time.monotonic()+30
  box=reference.getbbox();assert box,'No text rendered in Notepad'
  left,top,right,bottom=box
  while time.monotonic()<until:
   current=text_mask(shot(name),bounds)
   for advance in range(right-left+1,right-left+12):
    template=reference.crop((left,top,left+advance,bottom))
    if not same_pixels(template,current.crop((left,top,left+advance,bottom))):continue
    if duplicate:
     if not same_pixels(template,current.crop((left+advance,top,left+2*advance,bottom))):continue
     if current.crop((left+2*advance,0,current.width,current.height)).getbbox():continue
    elif current.crop((left+advance,0,current.width,current.height)).getbbox():continue
    return
   log();time.sleep(.5)
  raise AssertionError('Notepad text/copy-paste pixels did not match: '+name)
 def capture_stall(probe_pixel=None):
  # Preserve sleeping/runnable contexts when investigating a stalled redraw.
  # This runs only after the measured deadline, never in a successful sample.
  probe=out/'failure-task-layout.c'
  probe.write_text('#include "kernel/include/task.h"\n#include "gui/gui.h"\nextern int printf(const char *,...);\n#define O(m) ((unsigned)__builtin_offsetof(task_t,m))\n#define D(m) ((unsigned)__builtin_offsetof(gui_desktop_t,m))\n#define W(m) ((unsigned)__builtin_offsetof(gui_window_t,m))\nint main(void){printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",TASK_MAX,sizeof(task_t),O(pid),O(process_id),O(state),O(context),O(stack),O(stack_size),O(name),O(running_cpu),O(wake_tick),O(upcall_head),O(upcall_tail),O(upcall_active),O(window),D(focused_window),W(content_cache),W(content_cache_width),W(content_cache_height),W(content_ready));}\n')
  subprocess.run(['gcc','-m32','-I'+str(ROOT),str(probe),'-o',str(out/'failure-task-layout')],check=True)
  n,ts,*offsets=map(int,subprocess.check_output([str(out/'failure-task-layout')],text=True).split())
  tb=int(re.search(r'^([0-9a-f]+) b tasks$',symbols,re.M)[1],16)
  cmd('stop')
  try:
   raw=memory(tb,n*ts);rows=[]
   for tid in range(n):
    row=raw[tid*ts:(tid+1)*ts]
    pid,process,state,context,stack,size=[struct.unpack_from('<I',row,o)[0] for o in offsets[:6]]
    if not pid:continue
    item={'pid':pid,'process':process,'state':state,'context':context,'stack':stack,'stack_size':size,'name':row[offsets[6]:offsets[6]+24].split(b'\0')[0].decode(errors='replace'),'cpu':struct.unpack_from('<i',row,offsets[7])[0]}
    item.update(wake_tick=struct.unpack_from('<I',row,offsets[8])[0],upcall_head=row[offsets[9]],upcall_tail=row[offsets[10]],upcall_active=bool(row[offsets[11]]),window=struct.unpack_from('<I',row,offsets[12])[0])
    if stack and size and context>=stack and context<stack+size:
     data=memory(context,min(stack+size-context,16384));(out/f'failure-stack-{pid}.bin').write_bytes(data)
    rows.append(item)
   (out/'failure-tasks.json').write_text(json.dumps(rows,indent=2)+'\n')

   locks={}
   for label in ['g_desktop_paint_lock','g_bfs_lock','g_block_io_lock','g_ata_primary_lock']:
    match=re.search(r'^([0-9a-f]+) b '+label+r'$',symbols,re.M)
    if match:
     owner,depth,_,waiters,contentions=struct.unpack('<IHHII',memory(int(match[1],16),16))
     locks[label]={'owner':owner,'depth':depth,'waiters':waiters,'contentions':contentions}
   match=re.search(r'^([0-9a-f]+) b g_domains$',symbols,re.M)
   if match:
    data=memory(int(match[1],16),9*16)
    for i,label in enumerate(['task','vfs','gui','gfx','net','audio','driver','wine','legacy']):
     owner,depth,_,waiters,contentions=struct.unpack_from('<IHHII',data,i*16)
     locks[label]={'owner':owner,'depth':depth,'waiters':waiters,'contentions':contentions}
   (out/'failure-locks.json').write_text(json.dumps(locks,indent=2)+'\n')

   state={'probe_previous_pixel':list(probe_pixel) if probe_pixel else None,'windows':[]}
   for label in ['kernel_ticks','g_vision_frame_tid','g_vision_waiting_tid','kbd_evt_head','kbd_evt_tail','kbd_buf_head','kbd_buf_tail']:
    match=re.search(r'^([0-9a-f]+) [bB] '+label+r'$',symbols,re.M)
    if match:state[label]=struct.unpack('<I',memory(int(match[1],16),4))[0]
   desktop=struct.unpack('<I',memory(desktop_address,4))[0]
   state['focused_window']=struct.unpack('<I',memory(desktop+offsets[13],4))[0]
   pointer=struct.unpack('<I',memory(desktop+layout['first'],4))[0];seen=set()
   while pointer and pointer not in seen and len(seen)<128:
    seen.add(pointer);w=memory(pointer,layout['window_size'])
    info={'handle':pointer,'title':w[layout['title']:layout['title']+48].split(b'\0')[0].decode(errors='replace'),'visible':bool(w[layout['visible']]),'listed':bool(w[layout['listed']]),'content_ready':bool(w[offsets[17]])}
    cache=struct.unpack_from('<I',w,offsets[14])[0];info['cache']=cache
    if 0<cache<a.ram*1024*1024-4:info['cache_first_pixel']=list(memory(cache,4))
    state['windows'].append(info);pointer=struct.unpack_from('<I',w,layout['next'])[0]
   state['pic']=cmd('human-monitor-command',{'command-line':'info pic'})
   (out/'failure-input-state.json').write_text(json.dumps(state,indent=2)+'\n')


   registers=[cmd('human-monitor-command',{'command-line':'info registers','cpu-index':cpu}) for cpu in range(a.smp)]
   (out/'failure-registers.txt').write_text('\n'.join(registers))
   for cpu,register in enumerate(registers):
    match=re.search(r'ESP=([0-9A-Fa-f]+)',register)
    if match and 'CPL=0' in register:
     sp=int(match[1],16)
     owner=next((t for t in rows if t['cpu']==cpu and t['stack']<=sp<t['stack']+t['stack_size']),None)
     if owner:
      data=memory(sp,min(16384,owner['stack']+owner['stack_size']-sp));(out/f'failure-cpu{cpu}-stack.bin').write_bytes(data)

  finally:cmd('cont')
 wait_marker('[WINEUITEST] Notepad launch=1')
 launch_observed_host=time.monotonic()
 profile_before=None
 if a.profile_w3:
  profile_address=int(re.search(r'^([0-9a-f]+) b w3p_stats$',symbols,re.M)[1],16)
  profile_clock=int(re.search(r'^([0-9a-f]+) b w3p_last_clock$',symbols,re.M)[1],16)
  def profile_snapshot():
   cmd('stop')
   try:return memory(profile_address,540*44),struct.unpack('<Q',memory(profile_clock,8))[0],time.monotonic()
   finally:cmd('cont')
  profile_before=profile_snapshot()
 if a.load_latency:
  monitor=find_window('Wine y Vision')
  mx,my,mw,mh=monitor['bounds']
  modes=re.findall(r'ACTIVATE OK[^\n]*mode=(\d+)x(\d+)x32 pitch=(\d+) fb=(0x[0-9a-fA-F]+)',log())
  if not modes:raise RuntimeError('Load latency needs a 32-bit framebuffer')
  _,_,pitch,fb=modes[-1];pitch,fb=int(pitch),int(fb,16)
  pixel_address=fb+(my+100)*pitch+(mx+40)*4
  def native_pixel():return memory(pixel_address,4)
  native_desktop=struct.unpack('<I',memory(desktop_address,4))[0]
  def native_focused():
   return struct.unpack('<I',memory(native_desktop+layout['focused_window'],4))[0]==monitor['handle']
  # The fixture is the foreground native window until Notepad maps.
  load_samples=[]
  while '[WINE:TIME] first-surface' not in log() and time.monotonic()<deadline:
   if not native_focused():break
   old=native_pixel();before=time.monotonic()
   cmd('send-key',{'keys':[{'type':'qcode','data':'f7'}],'hold-time':30})
   until=before+2;changed=False
   while time.monotonic()<until:
    if native_pixel()!=old:changed=True;break
    time.sleep(.005)
   exceeded_two_seconds=not changed
   focused=native_focused();focus_checked=time.monotonic()
   if not changed and focused and a.load_probe and not (out/'failure-tasks.json').exists():
    shot('load-stall');capture_stall(old)
   # Never inject the next probe while an earlier press remains unacknowledged.
   # Otherwise a late repaint could be attributed to the following key.
   while not changed and focused and time.monotonic()<before+30:
    if time.monotonic()-focus_checked>.1:
     focused=native_focused();focus_checked=time.monotonic()
     if not focused:break
    if '[WINE:TIME] first-surface' in log():break
    if native_pixel()!=old:changed=True;break
    time.sleep(.01)
   # Window creation can transfer focus before its first surface is published.
   # Censor at the actual native focus change, not just the Wine log marker.
   if not native_focused() or '[WINE:TIME] first-surface' in log():
    load_samples.append({'host_s':before,'observed_ms':(time.monotonic()-before)*1000,
                         'changed':changed,'censored_at_focus_transfer':True,
                         'exceeded_two_seconds':exceeded_two_seconds,'diagnostic':a.load_probe})
    (out/'load-latency.json').write_text(json.dumps(load_samples,indent=2)+'\n')
    break
   load_samples.append({'host_s':before,'key_to_native_pixel_ms':(time.monotonic()-before)*1000,'changed':changed,'exceeded_two_seconds':exceeded_two_seconds,'diagnostic':a.load_probe})
   (out/'load-latency.json').write_text(json.dumps(load_samples,indent=2)+'\n')
   if not changed:break
   time.sleep(.3)
  shot('native-during-load')
 if a.startup_probe:
  observed_busy=False;captured=False;busy_samples=[]
  until=time.monotonic()+180
  while time.monotonic()<until:
   desktop=struct.unpack('<I',memory(desktop_address,4))[0]
   busy=struct.unpack('<I',memory(desktop+layout['busy_count'],4))[0]
   busy_samples.append({'host_s':time.monotonic(),'count':busy})
   if busy:
    observed_busy=True
    if not captured:shot('startup-busy');captured=True
   complete='[WINE:STAGE] TARGET exited raw=' if a.launch_failure else '[WINE:TIME] first-surface'
   if complete in log() and busy==0:break
   time.sleep(.25)
  else:raise TimeoutError('startup busy cursor did not clear')
  assert observed_busy,'busy cursor never became active during Wine startup'
  (out/'startup-busy.json').write_text(json.dumps(busy_samples,indent=2)+'\n')
  if a.launch_failure:
   status=re.search(r'TARGET exited raw=(\d+) code=(-?\d+)',log())
   assert status and int(status[2])!=0,'missing EXE unexpectedly succeeded'
   result={'result':'PASS','launch_failure':True,'busy_seen':observed_busy,'busy_cleared':busy==0,'exit_code':int(status[2])}
   (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
   shot('startup-failure-cleared');cmd('quit');proc.wait(timeout=10);s.close();print(json.dumps(result));sys.exit(0)
 wait_marker('[WINE:TIME] first-surface');surface_observed_host=time.monotonic()
 notepad=find_window('Notepad');print('Notepad mapped:',notepad,flush=True);shot('notepad-first-surface')
 if profile_before:
  after,clock,now=profile_snapshot();before,old_clock,then=profile_before;rows=[]
  for category in range(540):
   old=struct.unpack_from('<QQ7I',before,category*44);new=struct.unpack_from('<QQ7I',after,category*44)
   calls=(new[2]-old[2])&0xffffffff
   if not calls:continue
   cycles=new[0]-old[0]
   rows.append({'id':category,'calls':calls,'bytes':(new[3]-old[3])&0xffffffff,'cycles':cycles,'inclusive_host_ms':cycles*(now-then)*1000/(clock-old_clock)})
  rows.sort(key=lambda r:r['cycles'],reverse=True)
  (out/'profile-w3.json').write_text(json.dumps({'host_s':now-then,'note':'inclusive overlapping scopes, not sequential wall time; diagnostic build','rows':rows},indent=2)+'\n')
 if a.startup_only:
  first_surface=re.search(r'\[WINE:TIME\] first-surface ms=(\d+)',log())
  result={'result':'PASS','first_surface_ms':int(first_surface[1]),'ram_mib':a.ram,
          'host_launch_observed_to_surface_ms':(surface_observed_host-launch_observed_host)*1000,
          'smp':a.smp,'disk_bus':a.disk_bus,'vga':a.vga,
          'kernel_sha256':__import__('hashlib').sha256(kernel).hexdigest(),
          'notepad':notepad,'native_input_during_load':a.load_latency,
          'method':'guest launcher monotonic time to first surface; fresh guest boot, no typing into Wine'}
  (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  cmd('quit');proc.wait(timeout=10);s.close();print(json.dumps(result,indent=2));sys.exit(0)
 x,y,w,h=notepad['bounds'];click(x+70,y+90)
 if a.latency_samples:
  import statistics
  key('ctrl-a');key('backspace');time.sleep(1)
  samples=[];capture_ms=[]
  modes=re.findall(r'ACTIVATE OK[^\n]*mode=(\d+)x(\d+)x32 pitch=(\d+) fb=(0x[0-9a-fA-F]+)',log())
  if not modes:raise RuntimeError('Latency measurement requires a logged linear 32-bit framebuffer')
  fw,fh,pitch,fb=modes[-1];fw,fh,pitch,fb=int(fw),int(fh),int(pitch),int(fb,16)
  def capture_mask():
   before=time.monotonic();file=out/'latency-vram.bin'
   # Read only the displayed text scanlines. screendump refreshes and writes
   # the whole display, introducing substantial observer delay on this host.
   x,y,w,h=notepad['bounds']
   cmd('pmemsave',{'val':fb+(y+49)*pitch,'size':pitch*16,'filename':str(file)})
   strip=Image.frombytes('RGB',(pitch//4,16),file.read_bytes(),'raw','BGRX')
   mask=text_mask(strip,(x,-49,w,h))
   capture_ms.append((time.monotonic()-before)*1000)
   return mask
  previous=capture_mask()
  input_context=None
  if a.input_probe:
   ipc=(ROOT/'kernel/linux/i386_ipc.inc').read_text()
   structs=ipc[ipc.index('#define BLES_GFX_MAX_SURFACES'):ipc.index('static linux_device linux_devices')]
   probe_source=out/'input-offsets.c'
   probe_source.write_text('#include "kernel/include/klock.h"\n#include <stddef.h>\n#include <stdio.h>\n#include "ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h"\ntypedef void gui_window_t;\n'+structs+'\nint main(void){printf("%u %u %u",(unsigned)sizeof(linux_device),(unsigned)offsetof(bles_gfx_context,width),(unsigned)offsetof(bles_gfx_context,read_pos));}\n')
   subprocess.run(['gcc','-m32','-I'+str(ROOT),str(probe_source),'-o',str(out/'input-offsets')],check=True)
   device_size,width_offset,queue_offset=map(int,subprocess.check_output([str(out/'input-offsets')],text=True).split())
   devices_address=int(re.search(r'^([0-9a-f]+) b linux_devices$',symbols,re.M)[1],16)
   devices=memory(devices_address,128*device_size)
   for offset in range(0,len(devices),device_size):
    refs,flags,kind,pointer=struct.unpack_from('<IIII',devices,offset)
    if refs and kind==3 and pointer and any(memory(pointer+width_offset,4)):
     input_context=pointer;break
   if input_context is None:raise RuntimeError('No live TinyX graphics context')
  def input_positions():
   return memory(input_context+queue_offset,2) if input_context is not None else None
  if a.input_probe:
   stream_pixels=struct.unpack('<I',memory(notepad['handle']+layout['stream'],4))[0]
   stream_width=struct.unpack('<H',memory(notepad['handle']+layout['stream_width'],2))[0]
   if not stream_pixels or not stream_width:raise RuntimeError('No published window stream')
  def stream_ink():
   data=memory(stream_pixels+(49-layout['title_height'])*stream_width*4,stream_width*4*16)
   strip=Image.frombytes('RGB',(stream_width,16),data,'raw','BGRX')
   mask=text_mask(strip,(-layout['border'],-49,notepad['bounds'][2],notepad['bounds'][3]))
   return sum(v!=0 for v in mask.getdata())
  for index in range(a.latency_samples):
   # A short press avoids auto-repeat while the display catches up. Release
   # runs in QEMU, independently of the observer's file/VRAM query latency.
   previous=capture_mask()
   old_ink=sum(v!=0 for v in previous.getdata())
   code='m' if index%2==0 else 'w'
   positions=input_positions();route_ms=consume_ms=publish_ms=None
   old_stream_ink=stream_ink() if positions is not None else 0
   before=time.monotonic()
   cmd('send-key',{'keys':[{'type':'qcode','data':code}],'hold-time':30})
   injection_ms=(time.monotonic()-before)*1000
   until=before+5
   while time.monotonic()<until:
    if positions is not None:
     observed=input_positions();at=(time.monotonic()-before)*1000
     if route_ms is None and observed[1]!=positions[1]:route_ms=at
     if consume_ms is None and observed[0]!=positions[0]:consume_ms=at
     if publish_ms is None and stream_ink()>=old_stream_ink+10:
      publish_ms=(time.monotonic()-before)*1000
    current=capture_mask()
    if sum(v!=0 for v in current.getdata())>=old_ink+10:break
    time.sleep(.002)
   else:
    failure={'sample':index,'key':code,'injection_ms':injection_ms,
             'native_route_ms':route_ms,'tinyx_consume_ms':consume_ms,
             'stream_publish_ms':publish_ms,'old_ink':old_ink,
             'current_ink':sum(v!=0 for v in current.getdata()),
             'queue_before':list(positions) if positions is not None else None,
             'queue_after':list(input_positions()) if positions is not None else None}
    (out/'latency-failure.json').write_text(json.dumps(failure,indent=2)+'\n')
    shot('latency-failure')
    capture_stall()
    raise AssertionError('No new glyph for latency sample '+str(index))
   elapsed_ms=(time.monotonic()-before)*1000
   samples.append({'key_to_glyph_ms':elapsed_ms,'injection_ms':injection_ms,
                   'native_route_ms':route_ms,'tinyx_consume_ms':consume_ms,
                   'stream_publish_ms':publish_ms})
   print('LATENCY',index,round(elapsed_ms,2),flush=True)
   (out/'latency-partial.json').write_text(json.dumps(samples,indent=2)+'\n')
   previous=current;time.sleep(.8+(index%5)*.017)
  values=sorted(row['key_to_glyph_ms'] for row in samples)
  latency={'samples':samples,'p50_ms':statistics.median(values),'p95_ms':values[max(0,__import__('math').ceil(.95*len(values))-1)],'max_ms':max(values),'capture_p50_ms':statistics.median(capture_ms),'capture_max_ms':max(capture_ms),'ram_mib':a.ram,'smp':a.smp,'vga':a.vga,'tinyx':str(a.tinyx),'framebuffer':hex(fb),'method':'host monotonic from QMP key-down until new glyph ink in displayed VRAM scanlines; no guest stop while timing; includes VRAM query and QMP overhead'}
  first_surface=re.search(r'\[WINE:TIME\] first-surface ms=(\d+)',log())
  latency.update(disk_bus=a.disk_bus,kernel_sha256=__import__('hashlib').sha256(kernel).hexdigest(),first_surface_ms=int(first_surface[1]) if first_surface else None)
  (out/'latency.json').write_text(json.dumps(latency,indent=2)+'\n');shot('latency-final')
  if a.latency_only:
   cmd('quit');proc.wait(timeout=10);s.close();print(json.dumps(latency,indent=2));sys.exit(0)
  key('ctrl-a');key('backspace')
 for c in 'blesphasefour':key(c)
 # TinyX paints asynchronously; wait for the complete line and a stable
 # raster before recording the reference, rather than capturing a blank frame.
 reference=None;stable_since=0;baseline_deadline=time.monotonic()+45
 while time.monotonic()<baseline_deadline:
  sample=text_mask(shot('notepad-single'),notepad['bounds']);box=sample.getbbox()
  if box and box[2]-box[0]>=80:
   if reference is not None and same_pixels(reference,sample):
    if time.monotonic()-stable_since>=3:break
   else:reference=sample;stable_since=time.monotonic()
  else:reference=None;stable_since=0
  log();time.sleep(.5)
 else:raise AssertionError('The full typed Notepad line did not finish painting')
 key('ctrl-a');key('ctrl-c');time.sleep(1);key('end');key('ctrl-v')
 wait_text('notepad-copy-paste',notepad['bounds'],reference,True)
 monitor=find_window('Wine y Vision');mx,my,mw,mh=monitor['bounds'];click(mx+30,my+70);key('f9');wait_marker('[WINEUITEST] Notepad keyboard and clipboard verified');key('f10')
 wait_marker('[WINEUITEST] replacement ready');notepad_after=find_window('Notepad')
 assert notepad_after['handle']==notepad['handle'],'Wine surface did not survive'
 x,y,w,h=notepad_after['bounds'];click(x+70,y+90);key('ctrl-a');key('ctrl-v')
 wait_text('notepad-clipboard-after-restart',notepad_after['bounds'],reference,False)
 key('end')
 for c in 'blesphasefour':key(c)
 wait_text('notepad-input-after-restart',notepad_after['bounds'],reference,True)
 monitor=find_window('Wine y Vision');mx,my,mw,mh=monitor['bounds'];click(mx+30,my+70);key('f8');wait_marker('[WINEUITEST] DONE result=PASS');shot('notepad-after-restart')
 result={'result':'PASS','ram_mib':a.ram,'smp':a.smp,'vga':a.vga,'notepad':notepad,'notepad_after_restart':notepad_after,'keyboard_clipboard_before_after':True,'compositor_cpl3':'compositor y ventanas CPL=3' in log()}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 cmd('quit');proc.wait(timeout=10);s.close()
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True);(out/'memory.bin').unlink(missing_ok=True)
