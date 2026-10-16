#!/usr/bin/env python3
"""Disposable Wine memory gate; QMP is available to the control companion."""
import argparse,json,os,re,shutil,struct,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--ram',type=int,default=1024)
p.add_argument('--kernel-dir',type=Path,help='frozen kernel/ELF/stage2 for baseline comparisons')
p.add_argument('--target',default='/SYSTEM/TEST/WZ8/INSTALL.BAT')
p.add_argument('--keep-image',action='store_true')
p.add_argument('--debug',default='err+all,trace+loaddll')
p.add_argument('--disk-dir',type=Path,help='optional host directory for the disposable raw disk')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
def run(args):subprocess.run(args,cwd=ROOT,check=True)
if not a.kernel_dir:run(['make','build/stage2.bin'])
kernel_dir=a.kernel_dir.resolve() if a.kernel_dir else ROOT/'build'
for name in ['kernel.elf','kernel.bin','stage2.bin']:shutil.copyfile(kernel_dir/name,out/name)
symbols=subprocess.check_output(['nm',str(out/'kernel.elf')],text=True)
address=int(re.search(r'^([0-9a-f]+) b g_desktop$',symbols,re.M)[1],16)
offsets=out/'offsets.c';offsets.write_text('''#include "gui/gui.h"
extern int printf(const char *,...);
#define O(t,m) ((unsigned)__builtin_offsetof(t,m))
int main(){printf("%u %u %u %u %u %u %u %u %u %u %u",O(gui_desktop_t,first_window),O(gui_desktop_t,mouse_x),O(gui_desktop_t,mouse_y),sizeof(gui_window_t),O(gui_window_t,bounds),O(gui_window_t,title),O(gui_window_t,next),O(gui_window_t,visible),O(gui_window_t,listed),O(gui_window_t,owner_pid),O(gui_window_t,controller_pid));}
''')
run(['gcc','-m32','-I'+str(ROOT),str(offsets),'-o',str(out/'offsets')])
layout=dict(zip(['first','mouse_x','mouse_y','size','bounds','title','next','visible','listed','owner','controller'],map(int,subprocess.check_output([str(out/'offsets')],text=True).split())))
probe=out/'memprobe.elf';fixture=out/'memory_smoke.o'
run(['gcc','-m32','-Os','-fno-pie','-no-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-nostdlib','-static','-Wl,--build-id=none',str(ROOT/'tests/userland/wine_memory_probe.c'),'-o',str(probe)])
run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-DWINEMEM_TARGET="'+a.target+'"','-c',str(ROOT/'tests/userland/wine_memory_smoke.c'),'-o',str(fixture)])
launcher_source=out/'wine-launch.c'
launcher_source.write_text((ROOT/'ports/tinyx/wine-launch.c').read_text().replace('diagnostics = getenv("BLES_WINE_DIAGNOSTICS")','diagnostics = 1 || getenv("BLES_WINE_DIAGNOSTICS")').replace('WINEDEBUG=-all','WINEDEBUG='+a.debug))
launcher=out/'wine-memory.ELF'
run(['gcc','-m32','-O2','-no-pie','-I'+str(ROOT/'ports/tinyx'),'-Wl,--disable-new-dtags','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,-rpath,/WINEGATE/lib',str(launcher_source),'-o',str(launcher),'-L/usr/lib/i386-linux-gnu','-l:libX11.so.6','-l:libXfixes.so.3'])
disk_dir=a.disk_dir.resolve() if a.disk_dir else out;disk_dir.mkdir(parents=True,exist_ok=True)
image=disk_dir/'test.img';run(['cp','--reflink=auto',str(a.base),str(image)])
with image.open('r+b') as f:
 f.seek(512);f.write((out/'stage2.bin').read_bytes());f.seek(9*512);f.write((out/'kernel.bin').read_bytes().ljust(1280*1024,b'\0'))
e=Editor(image);e.mkdir('/SYSTEM/TEST/WZ8')
e.update('/SYSTEM/TEST/WZ8/WZ32V800.EXE',(ROOT/'build/wine-memory/wz32v800.exe').read_bytes())
e.update('/SYSTEM/TEST/WZ8/INSTALL.BAT',b'@echo off\r\nZ:\\SYSTEM\\TEST\\WZ8\\SETUP.EXE Z:\\SYSTEM\\TEST\\WZ8\\SETUP.WZ\r\n')
for source in sorted((ROOT/'build/wine-memory/winzip8-setup').iterdir()):
 if not source.is_file():continue
 path='/SYSTEM/TEST/WZ8/'+source.name;e.update(path,source.read_bytes())
 i=e.find(path);v=e.inodes[i];v[5:8]=[956131200]*3;v[28]=0
 raw=bytearray(struct.pack(bfs4.INODE_FORMAT,*v));struct.pack_into('<I',raw,248,bfs4.crc(raw));e.table[i*256:(i+1)*256]=raw
for path,source in [('/SYSTEM/TEST/MEMPROBE.ELF',probe),('/SYSTEM/PROGRAMS/WINEMEM.BEX',fixture),('/WINEGATE/bin/wine-memory.ELF',launcher)]:e.update(path,source.read_bytes())
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Wine memory|/SYSTEM/PROGRAMS/WINEMEM.BEX||Disposable test\n')
e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\ntimeout=3600\npath=/SYSTEM/SCREENS/SSPIPES.SCV\n');e.finish();e.fd.close()
sock=Path('/tmp/bk-winemem-'+str(os.getpid())+'.sock');serial=out/'boot.log';serial.unlink(missing_ok=True)
command=['qemu-system-i386','-accel','kvm','-cpu','host','-smp','2','-m',str(a.ram),'-drive',f'file={image},format=raw,if=ide','-vga','std','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-nodefaults','-no-reboot','-no-shutdown']
# Keep the disposable disk writable so a normal quit can preserve an installed
# WinZip fixture. The user's main image is never opened by this QEMU.
proc=subprocess.Popen(command,stderr=(out/'qemu.log').open('w'))
(out/'vm.json').write_text(json.dumps({'pid':proc.pid,'sock':str(sock),'image':str(image),'ram':a.ram,'desktop':address,'layout':layout,'command':command},indent=2))
print('QMP available:',out/'vm.json',flush=True)
try:
 deadline=time.monotonic()+2700
 while proc.poll() is None and time.monotonic()<deadline:
  log=serial.read_text(errors='replace') if serial.exists() else ''
  if '[WINEMEM] DONE' in log:break
  if '[PANIC]' in log or '[WINEMEM] FAIL' in log:raise RuntimeError('Guest failure; see boot.log')
  time.sleep(.5)
 if time.monotonic()>=deadline:raise TimeoutError('Wine memory gate')
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=15)
 sock.unlink(missing_ok=True)
 if not a.keep_image:image.unlink(missing_ok=True)
