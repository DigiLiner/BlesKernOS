#!/usr/bin/env python3
"""Boot an isolated disposable clone and measure idle RAM or exercise native apps/colors."""
import argparse,json,re,socket,subprocess,sys,time,zlib
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--ram',type=int,default=32)
p.add_argument('--swap-working-set-mib',type=int,choices=[6,8],default=6)
swap_choice=p.add_mutually_exclusive_group()
swap_choice.add_argument('--swap',action='store_true',help='attach a freshly formatted isolated swap disk')
swap_choice.add_argument('--swap-file',action='store_true',help='reserve a swap file inside the isolated system disk')
p.add_argument('--smoke',action='store_true')
p.add_argument('--vision-fault',action='store_true',help='install a test-only Vision that faults while holding frame locks')
p.add_argument('--fixture',choices=['ram','preferences','benchmark','idle','quake','swap','runtime','filesystem','vision'],default='ram')
p.add_argument('--runtime-case',type=int,choices=range(1,34),help='run one ABI case; default runs the complete suite')
p.add_argument('--accel',default='tcg')
p.add_argument('--cpu',default='qemu32,+rdrand')
p.add_argument('--smp',type=int,default=1)
p.add_argument('--network',action='store_true')
p.add_argument('--default-devices',action='store_true',help='retain QEMU automatic devices when testing file swap')
p.add_argument('--device',action='append',default=[])
p.add_argument('--audiodev',help='explicit QEMU audio backend, useful with -nodefaults')
p.add_argument('--cdrom',type=Path,help='attach an installer ISO read-only')
p.add_argument('--installed',action='store_true',help='test the existing image without replacing its kernel')
p.add_argument('--vga',default='std')
p.add_argument('--out',type=Path,default=ROOT/'tests/kernel/evidence/ram32-2026-10-10')
a=p.parse_args()
if a.vision_fault and (a.installed or not a.smoke or a.fixture!='vision'):p.error('--vision-fault requires --smoke --fixture vision and a rebuilt image')
tag=f'{a.ram}m-{a.vga}-'+('smoke' if a.smoke else 'idle')+('-before' if a.installed else '')
marker='PREFTEST' if a.fixture=='preferences' else 'RAMTEST'
if a.fixture=='benchmark':marker='BENCHTEST';tag+='-benchmark'
if a.fixture=='idle':marker='IDLETEST';tag+='-idle'
if a.fixture=='preferences':tag+='-preferences'
if a.fixture=='quake':marker='QUAKETEST';tag+='-quake'
if a.fixture=='runtime':marker='RUNTIMETEST';tag+='-runtime'
if a.runtime_case:
 assert a.fixture=='runtime','--runtime-case requires --fixture runtime'
 tag+='-case'+str(a.runtime_case)
if a.fixture=='filesystem':marker='FSTEST';tag+='-filesystem'
if a.fixture=='vision':marker='VISIONTEST';tag+='-vision'+('-fault' if a.vision_fault else '')
if a.fixture=='swap':marker='SWAPTEST';tag+='-swap'+('-file' if a.swap_file else '-on' if a.swap else '-off')
out=a.out/tag;out.mkdir(parents=True,exist_ok=True)
image=out/'test.img';sock=Path('/tmp/bk-ram-'+tag+'-'+format(zlib.crc32(str(a.out.resolve()).encode()),'x')+'.sock');sock.unlink(missing_ok=True)
subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
if not a.installed:
 subprocess.run(['make','build/stage2.bin'],cwd=ROOT,check=True)
 with image.open('r+b') as f:
  f.seek(512);f.write((ROOT/'build/stage2.bin').read_bytes())
  kernel=(ROOT/'build/kernel.bin').read_bytes()
  f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'))
 e=Editor(image)
 for target,source in [('/SYSTEM/DRIVERS/PS2MOUSE.DVR','build/system/drivers/PS2MOUSE.DVR'),('/SYSTEM/SERVICES/VISION.BEX','build/system/services/vision.o'),('/SYSTEM/CONTROL/DISPLAY.CPL','build/system/control/display.o'),('/SYSTEM/CONTROL/MOUSE.CPL','build/system/control/mouse.o'),('/SYSTEM/PROGRAMS/FILE.BEX','build/programs/filebrowser.o'),('/SYSTEM/PROGRAMS/TEXTEDITOR.BEX','build/programs/texteditor.o'),('/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX','build/programs/processmanager.o')]:
  e.update(target,(ROOT/source).read_bytes())
 if a.vision_fault:
  subprocess.run(['make','build/tests/vision_fault.o'],cwd=ROOT,check=True)
  e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/tests/vision_fault.o').read_bytes())
 e.finish()
if a.smoke:
 obj=out/'ram_smoke.o'
 fixture_source={'ram':'ram_smoke.c','preferences':'display_preferences_smoke.c','benchmark':'perf_smoke.c','idle':'idle_smoke.c','quake':'quake_smoke.c','swap':'swap_smoke.c','runtime':'runtime_smoke.c','filesystem':'bfs4_smoke.c','vision':'vision_restart_smoke.c'}[a.fixture]
 subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-DRUNTIME_CASE='+str(a.runtime_case or 0),'-DVISION_EXPECT_FAULT='+str(int(a.vision_fault)),'-DSWAP_EXPECT_ENABLED='+str(int(a.swap or a.swap_file)),'-c',str(ROOT/'tests/userland'/fixture_source),'-o',str(obj)],check=True)
 gfx=out/'gfx_context_smoke.elf'
 subprocess.run(['gcc','-m32','-Os','-fno-pie','-no-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-nostdlib','-static','-Wl,--build-id=none',str(ROOT/'tests/userland/gfx_context_smoke.c'),'-o',str(gfx)],check=True)
 if a.fixture=='runtime':
  runtime=ROOT/'build/phase12-runtime/WINEGATE'
  e=Editor(image)
  for directory in ['bin','lib']:
   for source in sorted((runtime/directory).iterdir()):
    if source.is_file():
     e.update('/WINEGATE/'+directory+'/'+source.name,source.read_bytes())
     if directory=='bin':e.update('/WINEGATE/bin/'+source.name.upper()+'.ELF',source.read_bytes())
  e.update('/WINEGATE/pc-large.dat',bytes([37])*(9*1024*1024))
  e.finish()
 if a.fixture=='swap':
  swap_elf=out/'swap_pressure.elf'
  subprocess.run(['gcc','-m32','-Os','-fno-pie','-no-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-nostdlib','-static','-Wl,--build-id=none','-DSWAP_PRESSURE_MIB='+str(a.swap_working_set_mib),str(ROOT/'tests/userland/swap_pressure.c'),'-o',str(swap_elf)],check=True)
  e=Editor(image);e.update('/SYSTEM/PROGRAMS/SWAPTEST.ELF',swap_elf.read_bytes());e.finish()
 e=Editor(image);e.update('/SYSTEM/PROGRAMS/GFXALLOC.ELF',gfx.read_bytes());e.update('/SYSTEM/PROGRAMS/RAMTEST.BEX',obj.read_bytes())
 if a.fixture=='quake':
  quake=ROOT/'extras/Quake-BlesKernOS-Standalone'
  e.mkdir('/QUAKE/ID1');e.update('/QUAKE/QUAKE.O',(quake/'build/QUAKE.O').read_bytes())
  e.update('/QUAKE/ID1/PAK0.PAK',(quake/'pak0.pak').read_bytes())
 e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|RAM test|/SYSTEM/PROGRAMS/RAMTEST.BEX||Native memory test\n');e.finish()
if a.swap_file:
 subprocess.run([sys.executable,str(ROOT/'tools/install_swap_file.py'),str(image),'--mib','32'],check=True)
elif a.fixture=='swap' and not a.swap:
 e=Editor(image);e.remove('/SYSTEM/SWAP.BKS');e.finish()
serial=out/'boot.log';serial.unlink(missing_ok=True)
swap_disk=out/'swap.img'
swap_disk.unlink(missing_ok=True)
swap_args=[]
if a.swap:
 subprocess.run([sys.executable,str(ROOT/'tools/mkswap_image.py'),str(swap_disk),'--mib','32'],check=True)
 swap_args=['-drive',f'file={swap_disk},format=raw,if=ide,index=2']
proc=subprocess.Popen(['qemu-system-i386','-accel',a.accel,'-cpu',a.cpu,'-smp',str(a.smp),'-drive',f'file={image},format=raw,if=ide','-m',str(a.ram),'-vga',a.vga,'-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown']+(['-nodefaults'] if a.swap_file and not a.default_devices else [])+swap_args+(['-audiodev',a.audiodev] if a.audiodev else [])+(['-netdev','user,id=net0','-device','rtl8139,netdev=net0'] if a.network else [])+[arg for device in a.device for arg in ['-device',device]]+(['-drive',f'file={a.cdrom.resolve()},format=raw,media=cdrom,if=ide'] if a.cdrom else []),stderr=(out/'qemu.log').open('w'))
try:
 deadline=time.monotonic()+(1800 if a.fixture=='runtime' else 900 if a.fixture=='swap' else 300 if a.fixture=='vision' else 150);done=False
 while time.monotonic()<deadline:
  log=serial.read_text(errors='replace') if serial.exists() else ''
  done=f'[{marker}] DONE' in log if a.smoke else ('[RAM:IDLE]' in log if not a.installed else 'DESKTOP RELEASED' in log)
  if done:break
  if 'simbolo no resuelto' in log or '[PANIC]' in log or ('[TASK] excepcion' in log and not a.vision_fault):raise RuntimeError('Guest failed; see '+str(serial))
  if proc.poll() is not None:raise RuntimeError('QEMU exited: '+(out/'qemu.log').read_text())
  time.sleep(.2)
 time.sleep(4)
 s=socket.socket(socket.AF_UNIX);s.settimeout(10);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
 def cmd(name,arguments=None):
  f.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
  while True:
   r=json.loads(f.readline())
   if 'error' in r:raise RuntimeError(r)
   if 'return' in r:return r['return']
 cmd('qmp_capabilities');cmd('screendump',{'filename':str(out/'desktop.ppm')})
 Image.open(out/'desktop.ppm').save(out/'desktop.png')
 cmd('quit');s.close();proc.wait(timeout=10)
 log=serial.read_text(errors='replace')
 # SMP serial writes can interleave the release message. The engine runs
 # only after release; together with the idle sample it independently proves
 # that boot reached the operating desktop, without treating a torn log as a
 # successful guest operation.
 desktop_started='DESKTOP RELEASED' in log or ('compositor y ventanas CPL=3' in log and '[RAM:IDLE]' in log)
 # A deferred-start diagnostic is emitted even when Wine never starts.
 # Keep rejecting every other boot marker, including unknown messages.
 wine_boot_started=any('[WINE:BOOT]' in line and
                       line.strip()!='[WINE:BOOT] deferred until Windows program launch'
                       for line in log.splitlines())
 result={'ram_mib':a.ram,'backend':a.vga,'desktop_started':desktop_started,'completed':done,
         'wine_boot_started':wine_boot_started,
         'smoke_pass':(f'[{marker}] DONE result=PASS' in log) if a.smoke else None,
         'memory':{},'depths':[int(n) for n in re.findall(r'RAMTEST:DEPTH\] bpp=(\d+) result=PASS',log)]}
 for stage,values in re.findall(r'\[RAM:(DESKTOP|IDLE)\] ([^\n]+)',log):
  result['memory'][stage]={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',values)}
 if a.smoke and a.fixture=='preferences':
  result['colors']=[{'wanted':int(w),'actual':int(c)} for w,c in re.findall(r'PREFTEST:DEPTH\] wanted=(\d+) actual=(\d+)',log)]
  assert log.count('[VISION:PREFS] applied')>=2,'Vision did not consume desktop preferences'
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2))
 assert done and result['desktop_started'],'Desktop/test did not finish'
 assert not a.smoke or result['smoke_pass'],'Native smoke test failed'
 if a.smoke and a.fixture=='quake':
  assert 'Quake Initialized' in log and '[quake] ERROR:' not in log,'Quake engine did not initialize'
 if a.vision_fault:
  injected=re.findall(r'VISION:FAULTTEST\] injecting UD2 while holding frame locks pid=(\d+)',log)
  faults=re.findall(r'\[TASK\] excepcion Ring 3 int=(\d+) error=\w+ eip=\w+ pid=(\d+)',log)
  assert len(injected)==1 and faults==[('6',injected[0])], 'Unexpected or missing server exception'
 assert not wine_boot_started,'Wine started at boot'
 assert 'PAGE FAULT' not in log and '[PANIC]' not in log,'Kernel fault'
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True);swap_disk.unlink(missing_ok=True)
