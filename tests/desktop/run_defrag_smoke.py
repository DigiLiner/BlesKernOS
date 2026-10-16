#!/usr/bin/env python3
"""Check Start stacking and run the real defragmenter through its Ring 3 UI."""
import argparse,hashlib,json,re,socket,struct,subprocess,sys,time
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor

def fragment_fixture(editor):
 payload=bytes((i*37+19)&255 for i in range(67*4096))
 path='/SYSTEM/TESTS/DEFRAG-FIXTURE.BIN';editor.mkdir('/SYSTEM/TESTS');editor.update(path,payload)
 number=editor.find(path);inode=editor.inodes[number];first=inode[10]
 count=33;run=0;second=None
 for b in range(editor.super[13],editor.super[3]):
  run=0 if bfs4.bit_get(editor.bitmap,b) else run+1
  if run==count:second=b+1-count;break
 assert second is not None
 editor.write(second,payload[34*4096:])
 for b in range(second,second+count):bfs4.bit_set(editor.bitmap,b)
 for b in range(first+34,first+67):bfs4.bit_set(editor.bitmap,b,False)
 inode[9]=2;inode[10:26]=[first,34,second,33]+[0]*12;inode[28]=0
 raw=bytearray(struct.pack(bfs4.INODE_FORMAT,*inode));struct.pack_into('<I',raw,248,bfs4.crc(raw))
 editor.table[number*256:(number+1)*256]=raw
 return payload,path

def run(memory,cpus,out,timeout):
 label=f'{memory}m-{cpus}cpu';out.mkdir(parents=True,exist_ok=True)
 image=ROOT/f'build/desktop-defrag-{label}.img'
 subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-admin.img'),str(image)],check=True)
 e=Editor(image)
 e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',
  b'3|0|2|2|1|1|180000|Wine|/WINEGATE/bin/wine-launch.ELF||Windows\n'
  b'4|1|1|10|0|1|30000|Process Manager|/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX||UI test\n')
 e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');payload,fixture=fragment_fixture(e);e.finish();bfs4.check(image)
 symbols=subprocess.check_output(['nm',str(ROOT/'build/kernel.elf')],text=True)
 address=int(re.search(r'^([0-9a-f]+) b g_defrag$',symbols,re.M).group(1),16)+4
 serial=out/f'boot-{label}.log';serial.unlink(missing_ok=True)
 sock=Path(f'/tmp/bk-desktop-defrag-{label}.sock');sock.unlink(missing_ok=True)
 proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-m',str(memory),'-smp',str(cpus),
  '-drive',f'file={image},format=raw,if=ide','-vga','std','-display','none','-serial',f'file:{serial}',
  '-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=(out/f'qemu-{label}.log').open('w'))
 client=None;start=time.monotonic()
 try:
  while not sock.exists() and time.monotonic()-start<20:time.sleep(.1)
  client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(10);client.connect(str(sock));stream=client.makefile('rwb');stream.readline()
  def command(ex,args=None):
   stream.write((json.dumps({'execute':ex,'arguments':args or {}})+'\n').encode());stream.flush()
   while True:
    response=json.loads(stream.readline())
    if 'error' in response:raise RuntimeError(response['error'])
    if 'return' in response:return response['return']
  command('qmp_capabilities')
  def hmp(value):return command('human-monitor-command',{'command-line':value})
  def report():
   raw=hmp(f'xp /19wx 0x{address:x}');values=[]
   for line in raw.splitlines():
    if ':' in line:values += [int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{8})',line.split(':',1)[1])]
   assert len(values)==19,raw
   return dict(zip(['state','error','owner','optimize','total','free','block_size','data_start','work','processed','verified','files','before','after','moved','skipped','inode','block','failed'],values))
  def shot(name):
   p=out/f'{name}-{label}.ppm';command('screendump',{'filename':str(p.resolve())});frame=Image.open(p).convert('RGB');frame.save(p.with_suffix('.png'));return frame
  def wait_for(predicate,seconds):
   until=time.monotonic()+seconds
   while time.monotonic()<until:
    if predicate():return
    if proc.poll() is not None:raise RuntimeError('QEMU exited')
    time.sleep(.25)
   raise TimeoutError('Timed out waiting for desktop condition')
  wait_for(lambda:serial.exists() and 'DESKTOP RELEASED' in serial.read_text(errors='replace'),90)
  time.sleep(3)
  before=shot('desktop-before-start')
  for _ in range(25):hmp('mouse_move -40 -40');time.sleep(.025)
  position=[0,0]
  def move(x,y):
   while position!=[x,y]:
    dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]));hmp(f'mouse_move {dx} {dy}');position[0]+=dx;position[1]+=dy;time.sleep(.04)
   time.sleep(.2)
  def click(x,y):
   move(x,y);hmp('mouse_button 1');time.sleep(.12);hmp('mouse_button 0');time.sleep(.8)
  click(400,555);time.sleep(1)
  menu=shot('start-over-windows')
  # The window's white table covers this region before Start is opened.
  face=(192,192,192) if memory>=96 else (219,219,255)
  region=menu.crop((350,240,510,370));pixels=list(region.getdata())
  for attempt in range(30):
   if sum(p==face for p in pixels)>len(pixels)*.65:break
   time.sleep(.5);menu=shot('start-over-windows');pixels=list(menu.crop((350,240,510,370)).getdata())
  assert sum(p==face for p in pixels)>len(pixels)*.65,'Start menu is behind the application'
  # Este equipo's formerly automatic submenu would cover this whole region.
  a=list(before.crop((550,260,700,370)).getdata());b=list(menu.crop((550,260,700,370)).getdata())
  assert sum(x==y for x,y in zip(a,b))>len(a)*.98,'Este equipo expanded without input'
  time.sleep(2);again=shot('start-after-window-repaint')
  region=again.crop((350,240,510,370));pixels=list(region.getdata())
  assert sum(p==face for p in pixels)>len(pixels)*.65,'Window repaint covered Start'
  print(f'PASS {label}: Start overlays repainting windows; Este equipo stays closed',flush=True)
  click(390,355);shot('start-programs')
  move(640,355);time.sleep(.5);click(640,165);time.sleep(2);initial=shot('defrag-initial')
  # A new window at (70,45), with the classic colored block grid.
  region=initial.crop((95,165,705,315));pixels=list(region.getdata())
  assert sum(120<=p[0]<=200 and abs(p[0]-p[1])<5 and abs(p[1]-p[2])<32 for p in pixels)>500,'Defrag block map did not render'
  click(140,503)
  wait_for(lambda:report()['state']==1,15)
  wait_for(lambda:report()['processed']>128,20)
  shot('defrag-analysis-progress')
  click(385,503);shot('defrag-stop-click');print('Stop clicked:',report(),flush=True);wait_for(lambda:report()['state']==3,10);cancelled=report();shot('defrag-analysis-stopped')
  click(255,503);wait_for(lambda:report()['state']==1,15)
  wait_for(lambda:report()['processed']>128,20);shot('defrag-moving-blocks')
  until=time.monotonic()+timeout
  while report()['state']==1 and time.monotonic()<until:
   time.sleep(.5)
  final=report();shot('defrag-complete')
  assert final['state']==2 and final['error']==0,final
  assert final['moved']>=1 and final['after']<final['before'],final
  # Red is reserved for actual I/O failures (covered by the host fault tests).
  log=serial.read_text(errors='replace');assert 'PANIC' not in log and 'PAGE FAULT' not in log and 'ERROR: sin memoria' not in log
  command('quit');proc.wait(timeout=10)
  e=Editor(image);after=e.contents(fixture);extents=e.inodes[e.find(fixture)][9];e.fd.close()
  assert after==payload and extents==1,'Fragmented fixture changed or stayed fragmented'
  bfs4.check(image)
  result={'status':'pass','memory_mib':memory,'cpus':cpus,'cancelled':cancelled,'final':final,'fixture_sha256':hashlib.sha256(after).hexdigest(),'fixture_extents':extents}
  (out/f'result-{label}.json').write_text(json.dumps(result,indent=2)+'\n')
  print(f'PASS {label}: real Ring 3 analyze/cancel/defrag; {final["moved"]} files moved; fixture reduced to one extent with identical data',flush=True)
 finally:
  if proc.poll() is None:
   try:command('quit')
   except Exception:proc.terminate()
   try:proc.wait(timeout=10)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
  if client:client.close()
  sock.unlink(missing_ok=True)
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--memory',type=int,default=256);parser.add_argument('--cpus',type=int,default=2);parser.add_argument('--timeout',type=int,default=160)
 parser.add_argument('--output',type=Path,default=ROOT/'tests/desktop/evidence/start-defrag-2026-10-09')
 args=parser.parse_args();run(args.memory,args.cpus,args.output,args.timeout)
