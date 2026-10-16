#!/usr/bin/env python3
"""Observe an ordinary Wine launch without guest monitors or DLL tracing."""
import argparse,json,re,shutil,socket,struct,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--kernel-elf',type=Path,required=True)
p.add_argument('--ram',type=int,default=1024)
p.add_argument('--smp',type=int,choices=[1,2],default=2)
p.add_argument('--disk-dir',type=Path,required=True)
p.add_argument('--keep-running',action='store_true',help='leave the guest available for input after detecting WinZip')
p.add_argument('--window',default='winzip',help='visible window title to observe; supports native desktop regression fixtures')
p.add_argument('--user-hardware',action='store_true',help='VMware VGA, RTL8139, AC97 and QEMU default peripherals')
p.add_argument('--cdrom',type=Path,help='attach the supplied installer ISO read-only')
p.add_argument('--display',choices=['none','gtk'],default='none')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);disk=a.disk_dir.resolve();disk.mkdir(parents=True,exist_ok=True)
image=disk/'test.img'
if image.exists():p.error('use a new disposable disk directory')
subprocess.run(['cp','--reflink=auto',str(a.base.resolve()),str(image)],check=True)
shutil.copyfile(a.kernel_elf,out/'kernel.elf')
symbols=subprocess.check_output(['nm',str(out/'kernel.elf')],text=True);desktop=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
source=out/'offsets.c'
source.write_text('''#include "gui/gui.h"
extern int printf(const char *,...);
#define O(t,m) ((unsigned)__builtin_offsetof(t,m))
int main(void){printf("%u %u %u %u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_desktop_t,mouse_x),O(gui_desktop_t,mouse_y),sizeof(gui_window_t),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,next),O(gui_window_t,visible),O(gui_window_t,listed),O(gui_window_t,owner_pid),O(gui_window_t,controller_pid));}
''')
subprocess.run(['gcc','-m32','-I'+str(ROOT),str(source),'-o',str(out/'offsets')],check=True)
layout=dict(zip(['first','mouse_x','mouse_y','size','bounds','title','next','visible','listed','owner','controller'],map(int,subprocess.check_output([str(out/'offsets')],text=True).split())))
sock=Path('/tmp/bk-wine-quiet-'+str(__import__('os').getpid())+'.sock');serial=out/'boot.log';serial.unlink(missing_ok=True)
command=['qemu-system-i386','-accel','kvm','-cpu','host','-smp',str(a.smp),'-m',str(a.ram),'-drive',f'file={image},format=raw,if=ide','-vga','vmware' if a.user_hardware else 'std','-display',a.display,'-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown']
if a.user_hardware:command+=['-netdev','user,id=net0','-device','rtl8139,netdev=net0','-device','AC97']
else:command+=['-nodefaults']
if a.cdrom:command+=['-drive',f'file={a.cdrom.resolve()},format=raw,media=cdrom,if=ide']
started=time.monotonic();proc=subprocess.Popen(command,stderr=(out/'qemu.log').open('w'))
(out/'vm.json').write_text(json.dumps({'pid':proc.pid,'sock':str(sock),'image':str(image),'ram':a.ram,'smp':a.smp,'desktop':desktop,'layout':layout,'command':command},indent=2))
def request(f,name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
        r=json.loads(f.readline())
        if 'error' in r:raise RuntimeError(r)
        if 'return' in r:return r['return']
def windows(f):
    scratch=out/'gui-memory.bin'
    def memory(address,size):
        request(f,'pmemsave',{'val':address,'size':size,'filename':str(scratch)});d=scratch.read_bytes();scratch.unlink();return d
    request(f,'stop')
    try:
        root=struct.unpack('<I',memory(desktop,4))[0]
        if not root:return []
        pointer=struct.unpack('<I',memory(root+layout['first'],4))[0];rows=[];seen=set()
        while pointer:
            assert pointer not in seen and len(seen)<128;seen.add(pointer)
            d=memory(pointer,layout['size']);rows.append({'title':d[layout['title']:layout['title']+48].split(b'\0')[0].decode(errors='replace'),'visible':bool(d[layout['visible']]),'bounds':struct.unpack_from('<iiii',d,layout['bounds'])});pointer=struct.unpack_from('<I',d,layout['next'])[0]
        return rows
    finally:request(f,'cont')
try:
    while not sock.exists():
        if proc.poll() is not None:raise RuntimeError('QEMU exited')
        time.sleep(.1)
    def connect():
        s=socket.socket(socket.AF_UNIX);s.settimeout(20);s.connect(str(sock))
        f=s.makefile('rwb',buffering=0);f.readline();request(f,'qmp_capabilities')
        return s,f
    ready=None
    while time.monotonic()-started<600:
        time.sleep(5);s,f=connect()
        try:rows=windows(f)
        finally:f.close();s.close()
        log=serial.read_text(errors='replace')
        assert '[PANIC]' not in log and 'PAGE FAULT' not in log,'guest fault'
        if any(w['visible'] and a.window.lower() in w['title'].lower() for w in rows):ready=time.monotonic()-started;break
    s,f=connect();stats=request(f,'query-blockstats')[0]['stats'];log=serial.read_text(errors='replace')
    result={'winzip_visible':ready is not None,'seconds_from_qemu_start':ready,'windows':rows,'serial_bytes':len(log.encode()),'memory_monitor': '[WINEMEM]' in log,'dll_trace':'trace:loaddll' in log,'repeated_frame_trace':'[VISION:FRAME]' in log,'disk':{k:stats[k] for k in ['wr_bytes','rd_bytes','wr_operations','flush_operations']},'method':'ordinary launcher and WinZip startup; paused QMP window reads every five seconds; no guest profiling'}
    result['window_match']=a.window
    result['window_visible']=ready is not None
    if a.window!='winzip':
        result['winzip_visible']=False
        result['method']='native window startup; paused QMP window reads every five seconds; no guest profiling'
    (out/'result.json').write_text(json.dumps(result,indent=2));request(f,'screendump',{'filename':str(out/'desktop.ppm')})
    from PIL import Image
    Image.open(out/'desktop.ppm').save(out/'desktop.png');print(json.dumps(result,indent=2),flush=True)
    assert ready is not None,'WinZip did not appear'
    assert not any(result[k] for k in ['memory_monitor','dll_trace','repeated_frame_trace']),'diagnostic traffic remained enabled'
    if a.keep_running:
        f.close();s.close()
        while proc.poll() is None and time.monotonic()-started<1800:time.sleep(.5)
    else:request(f,'quit');s.close();proc.wait(timeout=15)
finally:
    if proc.poll() is None:proc.terminate();proc.wait(timeout=15)
    sock.unlink(missing_ok=True)
