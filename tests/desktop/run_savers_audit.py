#!/usr/bin/env python3
"""Measure and visually exercise the real savers on disposable BFS4 images."""
import argparse,atexit,json,re,subprocess,sys,time
from pathlib import Path
from PIL import Image,ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from run_medium_ui_smoke import UI

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 p.add_argument('--before-dir',type=Path)
 p.add_argument('--out',type=Path,default=ROOT/'tests/desktop/evidence/savers-2026-10-17')
 p.add_argument('--accel',choices=['tcg','kvm'],default='tcg')
 p.add_argument('--savers',nargs='+',choices=['logo','balls','pipes'],default=['logo','balls','pipes'])
 a=p.parse_args();tag='before' if a.before_dir else 'after'
 for name in a.savers:
  kind={'logo':1,'balls':2,'pipes':3}[name]
  out=a.out.resolve()/tag/name;out.mkdir(parents=True,exist_ok=True);(out/'result.json').unlink(missing_ok=True)
  raw=out/'probe.raw.o';obj=out/'probe.o'
  flags=['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-I'+str(ROOT/'libs/Mesa-3.5/include'),'-I'+str(ROOT/'libs/Mesa-3.5/bleskernos/include'),f'-DSAVER_KIND={kind}']
  if a.before_dir:flags+=['-DSAVER_BASELINE',f'-DSAVER_SOURCE="{a.before_dir.resolve()/f"ss_{name}.c"}"']
  subprocess.run(flags+['-c',str(ROOT/'tests/userland/saver_probe.c'),'-o',str(raw)],check=True)
  runtime=[] if a.before_dir else [str(ROOT/'build/system/screensavers/ss_runtime.o'),str(ROOT/'build/kernel/libc/math.o')]
  if a.before_dir:
   subprocess.run(['ld','-m','elf_i386','-r',str(raw),str(ROOT/'build/system/libs/mesa35/mesa35.a'),*runtime,'-o',str(obj)],check=True)
  else:
   subprocess.run(['python3',str(ROOT/'tools/generate_lbs_client.py'),'--source',str(raw),'--output',str(obj),'--destination','/SYSTEM/PROGRAMS/SAVERPROBE.BEX'],check=True)
  image=ROOT/f'build/saver-{tag}-{name}-test.img'
  atexit.register(image.unlink,missing_ok=True)
  subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
  e=Editor(image)
  for library in (ROOT/'build/system/libs/native').glob('*.LBS'):e.update('/SYSTEM/'+library.name,library.read_bytes())
  e.update('/SYSTEM/PROGRAMS/SAVERPROBE.BEX',obj.read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Saver probe|/SYSTEM/PROGRAMS/SAVERPROBE.BEX||Saver audit\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
  sock=ROOT/f'build/saver-{tag}-{name}.sock';sock.unlink(missing_ok=True);serial=out/'boot.log';serial.unlink(missing_ok=True)
  with (out/'qemu.log').open('w') as log:
   proc=subprocess.Popen(['qemu-system-i386','-accel',a.accel,'-cpu','host' if a.accel=='kvm' else 'qemu32','-m','256','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=log)
   try:
    for _ in range(360):
     if proc.poll() is not None:raise RuntimeError('QEMU exited: '+(out/'qemu.log').read_text())
     text=serial.read_text(errors='replace') if serial.exists() else ''
     if '[SAVERPROBE] FAIL' in text:raise RuntimeError(text[-3000:])
     if '[SAVERPROBE] live_frames=' in text:break
     time.sleep(.5)
    else:raise TimeoutError(name+' probe')
    bench=re.search(r'BENCH kind=\d+ frames=(\d+) render_ms=(\d+) blit_frames=(\d+) blit_ms=(\d+)',text)
    assert bench,text[-3000:]
    print(tag,name,'bench',bench.groups(),flush=True)
    time.sleep(.7);ui=UI(sock,out);ui.shot('01-active')
    first=Image.open(out/'01-active.png').convert('RGB');changed=0
    for attempt in range(5):
     time.sleep(2);ui.shot('02-animation');second=Image.open(out/'02-animation.png').convert('RGB')
     changed=sum(pixel!=(0,0,0) for pixel in ImageChops.difference(first,second).getdata())
     if changed>100:break
    black=sum(max(pixel)<32 for pixel in first.getdata())
    ui.key('esc')
    for attempt in range(80):
     if attempt and attempt % 8 == 0:ui.key('esc')
     if '[SAVERPROBE] CLOSED' in serial.read_text(errors='replace'):break
     time.sleep(.25)
    else:raise TimeoutError(name+' did not close')
    ui.shot('03-closed');closed=Image.open(out/'03-closed.png').convert('RGB')
    closed_black=sum(max(pixel)<32 for pixel in closed.getdata())
    text=serial.read_text(errors='replace')
    live=[list(map(int,row)) for row in re.findall(r'live_frames=(\d+) live_ms=(\d+) paint_ms=(\d+)',text)]
    result={'saver':name,'accel':a.accel,'frames':int(bench[1]),'render_ms':int(bench[2]),'blit_frames':int(bench[3]),'blit_ms':int(bench[4]),'changed_pixels':changed,'black_pixels':black,'closed_black_pixels':closed_black,'live_samples':live}
    assert '[PANIC]' not in text and '[TASK] excepcion' not in text,text[-3000:]
    if not a.before_dir:
     assert changed>100,(name,result)
     assert black>300000,(name,'does not cover desktop',result)
     assert closed_black<black//2,(name,'desktop not restored',result)
    result['status']='baseline' if a.before_dir else 'pass'
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
    ui.command('quit');proc.wait(timeout=10);ui.stream.close();ui.socket.close()
   finally:
    if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    sock.unlink(missing_ok=True)
  image.unlink(missing_ok=True)
if __name__=='__main__':main()
