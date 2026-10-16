#!/usr/bin/env python3
"""Exercise Win32 graphics through Wine and the real VirGL GPU in a disposable image."""
import argparse, hashlib, json, subprocess, sys, time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from bfs4_update import Editor
from run_qemu_virtio import graphics_environment
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=root/'build/bleskernos-travelmate250.img')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--native-glx',action='store_true',help='test GLX drawable semantics without Wine')
p.add_argument('--local-gl',action='store_true',help='install freshly built GLX and VirGL into the test copy')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
image=out/'test.img'
if image.exists():p.error('use a fresh output directory')
def run(*args):subprocess.run([str(v) for v in args],cwd=root,check=True)
pe=out/'games32.exe';launcher=out/'games32-launch.ELF'
run('i686-w64-mingw32-gcc','-O2','-Wall','-Wextra','-Werror',root/'ports/wine/tests/games32.c',root/'ports/wine/tests/direct3d7.c','-o',pe,'-ld3d9','-ld3d11','-ld3dcompiler','-lddraw','-ldxguid','-lopengl32','-lgdi32')
run('gcc','-m32','-O2','-Wall','-Wextra','-no-pie','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,--disable-new-dtags','-Wl,-rpath,/WINEGATE/lib',root/'ports/wine/tests/games32-launch.c','-o',launcher)
run('cp','--reflink=auto',a.base.resolve(),image)
e=Editor(image);e.update('/WINEGATE/bin/games32.exe',pe.read_bytes());e.update('/WINEGATE/bin/games32-launch.ELF',launcher.read_bytes())
e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\ntimeout=3600\n')
if a.local_gl:e.update('/WINEGATE/lib/libGL.so.1',(root/'ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib/libGL.so.1').read_bytes())
if a.local_gl:e.update('/WINEGATE/lib/libbkvirgl.so.8',(root/'build/wine/graphics/libbkvirgl.so.8').read_bytes())
if a.native_glx:
 native=out/'test-glx.ELF'
 run('gcc','-m32','-O2','-Wall','-Wextra',root/'ports/wine/graphics/test-glx.c','-o',native,'-L'+str(root/'ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib'),'-l:libGL.so.1','-l:libX11.so.6','-ldl','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,--disable-new-dtags','-Wl,-rpath,/WINEGATE/lib')
 e.update('/WINEGATE/bin/games32-launch.ELF',native.read_bytes())
e.finish()
run(sys.executable,'ports/tinyx/scripts/build-stage2-kernel.py',image,'--launcher','/WINEGATE/bin/games32-launch.ELF','--launch-delay','10')
command=['qemu-system-i386','-accel','kvm','-cpu','host','-smp','2','-m','1536','-drive',f'file={image},format=raw,if=ide','-vga','none','-device','virtio-vga-gl','-display','egl-headless,rendernode=/dev/dri/renderD128','-serial',f'file:{out}/serial.log','-no-reboot','-no-shutdown']
start=time.monotonic();serial='';passed=False;finished=False
with (out/'qemu.log').open('w') as log:
 process=subprocess.Popen(command,stderr=log,env=graphics_environment('virtio-vga-gl'))
 try:
  while time.monotonic()-start<360:
   path=out/'serial.log'
   if path.exists():serial=path.read_text(errors='replace')
   passed=('[GLX-PROBE] PASS' if a.native_glx else '[GAMES32] ALL PASS') in serial
   if '[GATE:STAGE2] RESULT' in serial:
    finished=True
    passed=passed and 'status=0' in serial.split('[GATE:STAGE2] RESULT')[-1];break
   if '[GLX-PROBE] FAILED' in serial or '[GAMES32] FAIL' in serial or '[PANIC]' in serial or process.poll() is not None:break
   time.sleep(.2)
 finally:
  process.terminate()
  try:process.wait(timeout=10)
  except subprocess.TimeoutExpired:process.kill();process.wait()
host_log=(out/'qemu.log').read_text(errors='replace')
gpu_errors=[line for line in host_log.splitlines() if 'Failed to create fence' in line or 'EGL error' in line]
passed=passed and finished and not gpu_errors
result={'finished':finished,'gpu_errors':gpu_errors,'result':'PASS' if passed else 'FAIL','host_seconds':time.monotonic()-start,'command':command,'pe_sha256':hashlib.sha256(pe.read_bytes()).hexdigest(),'checks':[line for line in serial.splitlines() if '[COMPAT:' in line or '[GAMES32]' in line or '[GLX-PROBE]' in line]}
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
sys.exit(0 if passed else 1)
