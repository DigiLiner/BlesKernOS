#!/usr/bin/env python3
"""Record an actual installed Win32 game in a private AHCI/VirGL guest."""
import argparse,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from run_qemu_virtio import graphics_environment
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--target',required=True)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--seconds',type=int,default=240)
p.add_argument('--trace',action='store_true')
p.add_argument('--virgl-sync',action='store_true',help='diagnose with a GPU completion fence on every Gallium submission')
p.add_argument('--wine-debug',default='-all,err+all')
p.add_argument('--runtime-perf',action='store_true',help='periodic kernel I/O and scheduler counters')
p.add_argument('--audio',action='store_true',help='AC97 captured into WAV')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
image=out/'test.img'
if image.exists():p.error('use a fresh output directory')
def run(*args):subprocess.run([str(v) for v in args],cwd=ROOT,check=True)
flags=['gcc','-m32','-O2','-no-pie','-I'+str(ROOT/'ports/tinyx'),'-Wl,--disable-new-dtags','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,-rpath,/WINEGATE/lib']
s=(ROOT/'ports/tinyx/wine-launch.c').read_text().replace('WINEDEBUG=-all','WINEDEBUG='+a.wine_debug)
if a.virgl_sync:s=s.replace('    "LP_NUM_THREADS=1",','    "LP_NUM_THREADS=1", "VIRGL_DEBUG=sync",')
if a.trace:s=s.replace('    "LP_NUM_THREADS=1",','    "LP_NUM_THREADS=1", "BLES_WINE_GL_TRACE=1",')
source=out/'wine-launch.c';source.write_text(s)
run(*flags,source,'-o',out/'wine-game.ELF','-l:libX11.so.6','-l:libXfixes.so.3')
source=out/'launch.c';source.write_text('#include <unistd.h>\nint main(void){execl("/WINEGATE/bin/wine-game.ELF","wine-game","--diagnostics",'+json.dumps(a.target)+',NULL);return 127;}\n')
run(*flags,source,'-o',out/'game-launch.ELF')
run('cp','--reflink=auto',a.base.resolve(),image)
e=Editor(image)
for name in ['game-launch','wine-game']:e.update('/WINEGATE/bin/'+name+'.ELF',(out/(name+'.ELF')).read_bytes())
e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\ntimeout=3600\n');e.finish()
run(sys.executable,'ports/tinyx/scripts/build-stage2-kernel.py',image,'--launcher','/WINEGATE/bin/game-launch.ELF','--launch-delay','10',*(['--runtime-perf'] if a.runtime_perf else []))
sock=Path('/tmp/bkgame-'+str(os.getpid())+'.sock')
command=['qemu-system-i386','-accel','kvm','-cpu','host','-smp','2','-m','1536',
 '-drive',f'file={image},format=raw,if=none,id=osdisk','-device','ich9-ahci,id=ahci','-device','ide-hd,drive=osdisk,bus=ahci.0',
 '-drive',f'file={a.iso.resolve()},media=cdrom,readonly=on','-vga','none','-device','virtio-vga-gl','-display','egl-headless,rendernode=/dev/dri/renderD128',
 '-serial',f'file:{out}/serial.log','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown']
if a.audio:command+=['-audiodev',f'wav,id=snd,path={out}/audio.wav','-device','AC97,audiodev=snd']
(out/'command.json').write_text(json.dumps({'command':command,'qmp':str(sock)},indent=2)+'\n')
process=subprocess.Popen(command,stderr=(out/'qemu.log').open('w'),env=graphics_environment('virtio-vga-gl'));start=time.monotonic()
print('QEMU',process.pid,'QMP',sock,flush=True)
try:
 while time.monotonic()-start<a.seconds:
  if process.poll() is not None:break
  time.sleep(.25)
finally:
 process.terminate()
 try:process.wait(timeout=10)
 except subprocess.TimeoutExpired:process.kill();process.wait()
 sock.unlink(missing_ok=True)
serial=(out/'serial.log').read_text(errors='replace');host=(out/'qemu.log').read_text(errors='replace')
(out/'observations.json').write_text(json.dumps({'elapsed':time.monotonic()-start,'target':a.target,'audio':a.audio,'host_log':host,'failure_markers':[line for line in serial.splitlines() if any(t in line for t in ['[PANIC]','timeout controlq','Unhandled page fault','[WINE] application failed'])]},indent=2)+'\n')
