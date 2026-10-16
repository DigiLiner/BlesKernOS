#!/usr/bin/env python3
"""Boot an isolated BFS4 copy and exercise administration through real Ring 3 APIs."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
FILES={
 '/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX':'build/programs/processmanager.o',
 '/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX':'build/programs/event_viewer.o',
 '/SYSTEM/PROGRAMS/FILE.BEX':'build/programs/filebrowser.o',
 '/SYSTEM/SERVICES/VISION.BEX':'build/system/services/vision.o',
 '/SYSTEM/SERVICES/PRINTSPL.BEX':'build/system/services/printspool.o',
 '/SYSTEM/TESTS/NATIVE/RING3PROXY.BEX':'build/programs/ring3proxy.o',
 '/SYSTEM/TESTS/NATIVE/ADMINTEST.BEX':'build/programs/admin_smoke.o',
 '/SYSTEM/CONTROL/CONTROL.BEX':'build/system/control/control_panel.o',
 '/SYSTEM/CONTROL/SYSTEM.CPL':'build/system/control/system.o',
 '/SYSTEM/CONTROL/DEVMGR.CPL':'build/system/control/device_manager.o',
 '/SYSTEM/USER/CONFIG/DESKTOP.INI':'config/desktop/Desktop.INI',
}
CONFIG=(b'3|0|2|2|1|1|180000|Wine|/WINEGATE/bin/wine-launch.ELF||Windows\n'
        b'4|1|1|1|1|1|30000|Admin test|/SYSTEM/TESTS/NATIVE/ADMINTEST.BEX||Integration test\n')

def prepare(base,image):
 subprocess.run(['cp','--reflink=auto',str(base),str(image)],check=True)
 with image.open('r+b') as f:
  f.seek(512);f.write((ROOT/'build/stage2.bin').read_bytes())
  f.seek(9*512);f.write((ROOT/'build/kernel.bin').read_bytes())
 e=Editor(image)
 for dst,src in FILES.items():e.mkdir(str(Path(dst).parent));e.update(dst,(ROOT/src).read_bytes())
 e.mkdir('/SYSTEM/USER/CONFIG');e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',CONFIG)
 e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()

def run(image,memory,cpus,out,timeout):
 label=f'{memory}m-{cpus}cpu';serial=out/f'boot-{label}.log';qmp=Path(f'/tmp/bk-admin-{label}.sock')
 qmp.unlink(missing_ok=True)
 serial.unlink(missing_ok=True)
 with (out/f'qemu-{label}.log').open('w') as stderr:
  proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-m',str(memory),'-smp',str(cpus),
   '-drive',f'file={image},format=raw,if=ide','-vga','std','-netdev','user,id=n0','-device','rtl8139,netdev=n0',
   '-display','none','-serial',f'file:{serial}','-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot'],stderr=stderr)
  client=None;captured=set();start=time.monotonic();log=''
  try:
   while time.monotonic()-start<timeout:
    if proc.poll() is not None:raise RuntimeError(f'QEMU exited: {proc.returncode}')
    if serial.exists():log=serial.read_text(errors='replace')
    if '[ADMINTEST]' in log and ': FAIL' in log:raise RuntimeError('Ring 3 fixture failed')
    if 'PANIC' in log or 'PAGE FAULT' in log:raise RuntimeError('kernel exception')
    for stage in range(2):
     if stage in captured or f'[ADMINTEST] UI {stage} visible' not in log:continue
     if client is None:
      client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(10);client.connect(str(qmp));stream=client.makefile('rwb');stream.readline()
      def command(payload):
       stream.write((json.dumps(payload)+'\n').encode());stream.flush()
       while True:
        response=json.loads(stream.readline())
        if 'error' in response:raise RuntimeError(response['error'])
        if 'return' in response:return response['return']
      command({'execute':'qmp_capabilities'})
     name=['processes','events'][stage];path=out/f'{name}-{label}.ppm'
     from PIL import Image
     ready=False
     for attempt in range(30):
      time.sleep(.2)
      command({'execute':'screendump','arguments':{'filename':str(path.resolve())}})
      frame=Image.open(path).convert('RGB')
      sample=frame.crop((100,300,500,370) if stage==0 else (100,200,600,300))
      target=(255,255,255) if stage==0 else (0,0,0)
      matching=sum(pixel==target for pixel in sample.getdata())
      if matching>sample.width*sample.height*.6:
       frame.save(path.with_suffix('.png'));ready=True;break
     if not ready:raise RuntimeError(f'{name} client frame did not render')
     captured.add(stage);print(f'{label}: captured {name}',flush=True)
    if '[ADMINTEST] UI ALL PASS' in log:
     assert '[RING3] proxy API y retorno a user-space: OK' in log
     assert '[ADMINTEST] ALL PASS' in log
     assert 'ERROR: sin memoria' not in log
     print(f'PASS {label}: services, worker threads, RAM, guards, network persistence, export and both UIs',flush=True)
     return
    time.sleep(.2)
   raise TimeoutError(f'{label} did not finish in {timeout}s')
  finally:
   if client:
    try:command({"execute":"quit"})
    except (OSError,ValueError,RuntimeError):proc.terminate()
   else:proc.terminate()
   try:proc.wait(timeout=10)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
   if client:client.close()
   qmp.unlink(missing_ok=True)

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 parser.add_argument('--memory',type=int,default=64);parser.add_argument('--cpus',type=int,default=1)
 parser.add_argument('--timeout',type=int,default=180)
 parser.add_argument('--output',type=Path,default=ROOT/'tests/kernel/evidence/admin-2026-10-09/final')
 args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
 image=ROOT/f'build/admin-smoke-{args.memory}m-{args.cpus}cpu.img'
 prepare(args.base,image);run(image,args.memory,args.cpus,args.output,args.timeout)
