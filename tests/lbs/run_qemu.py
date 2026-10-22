#!/usr/bin/env python3
"""Boot a disposable BFS4 image and run the actual LBS Ring 3 demonstration."""
import argparse,json,re,subprocess,sys,time,socket,threading,hashlib,wave,array
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from lbs_kernel import install_kernel
from lbs_payload import application_payloads, remove_old_libraries
p=argparse.ArgumentParser();p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img');p.add_argument('--memory',type=int,default=256);p.add_argument('--vga',default='std');p.add_argument('--timeout',type=int,default=180);p.add_argument('--keep-image',action='store_true');p.add_argument('--virgl',action='store_true');p.add_argument('--accel',choices=['tcg','kvm'],default='tcg');p.add_argument('--cpu');p.add_argument('--triangle',action='store_true');a=p.parse_args()
if a.virgl:a.vga='virgl'
label=f'{a.vga}-{a.memory}m'+('-kvm' if a.accel=='kvm' else '')+('-rdrand' if a.cpu else '')+('-triangle' if a.triangle else '')
out=ROOT/f'tests/lbs/evidence/{label}';out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
image=ROOT/f'build/lbs-{label}.img';serial=out/'serial.log';sock=ROOT/f'build/lbs-{label}.sock'
subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
echo=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);echo.bind(('127.0.0.1',0));echo.settimeout(.2)
echo_stop=threading.Event();echo_count=[0]
def echo_server():
 while not echo_stop.is_set():
  try:
   data,peer=echo.recvfrom(4096);echo.sendto(data,peer);echo_count[0]+=1
  except socket.timeout:pass
  except OSError:break
echo_thread=threading.Thread(target=echo_server,daemon=True);echo_thread.start()
inputs={str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in list((ROOT/'build/system/libs/native').glob('*.LBS'))+[ROOT/'build/tests/userland/LBSTEST.BEX',ROOT/'build/tests/userland/LBSTRI.BEX']}
with a.base.open('rb') as base_stream:inputs['boot_area_sha256']=hashlib.sha256(base_stream.read(4096*512)).hexdigest()
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
install_kernel(image)
with image.open('rb') as staged:inputs['boot_area_sha256']=hashlib.sha256(staged.read(4096*512)).hexdigest()
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
e=Editor(image);e.mkdir('/SYSTEM');e.mkdir('/SYSTEM/TESTS/NATIVE')
remove_old_libraries(e)
for guest,host in application_payloads().items():e.mkdir(str(Path(guest).parent));e.update(guest,host.read_bytes())
for f in (ROOT/'build/system/libs/native').glob('*.LBS'):e.update('/SYSTEM/'+f.name,f.read_bytes())
for f in (ROOT/'build/tests/lbs').glob('*.LBS'):e.update('/SYSTEM/TESTS/NATIVE/'+f.name,f.read_bytes())
e.update('/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX',(ROOT/'build/lbs/apps/programs/network_status.o').read_bytes())
from PIL import Image
import io,zipfile
gif=io.BytesIO();Image.new('RGB',(8,8),(220,30,70)).save(gif,format='GIF')
e.update('/SYSTEM/TESTS/NATIVE/FRAME.GIF',gif.getvalue())
zip_data=io.BytesIO()
with zipfile.ZipFile(zip_data,'w') as archive:
 archive.writestr('stored.txt',b'shared-code!',compress_type=zipfile.ZIP_STORED)
 archive.writestr('nested/deflated.txt',b'shared-code!',compress_type=zipfile.ZIP_DEFLATED)
e.update('/SYSTEM/TESTS/NATIVE/SHARED.ZIP',zip_data.getvalue())

# Small deterministic streaming fixtures exercise the public MEDIA API.
subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','color=c=blue:s=32x32:r=2',
                '-t','1','-c:v','rawvideo','-pix_fmt','bgr24','-threads','1',str(out/'STREAM.AVI')],check=True)
subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i','color=c=red:s=32x32:r=25',
                '-f','lavfi','-i','sine=frequency=440:sample_rate=44100','-t','0.4',
                '-c:v','mpeg1video','-c:a','mp2','-threads','1','-f','mpeg',str(out/'STREAM.MPG')],check=True)
for fixture in ['STREAM.AVI','STREAM.MPG']:
 e.update('/SYSTEM/TESTS/NATIVE/'+fixture,(out/fixture).read_bytes())
e.update('/SYSTEM/TESTS/NATIVE/DEMO.BKV',(ROOT/'programs/viewer/examples/DEMO60.BKV').read_bytes())

e.update('/SYSTEM/TESTS/NATIVE/UDPPORT.TXT',str(echo.getsockname()[1]).encode())
e.update('/SYSTEM/CORRUPT.LBS',b'not an ELF library')
for guest,host in [('POSIX_TEST.BEX','build/programs/posix_test.o'),('SDL_DEMO.BEX','build/programs/sdl_demo.o'),('DLMOD.SO','build/tests/userland/DLMOD.SO'),('DLDEP.SO','build/tests/userland/DLDEP.SO')]:
 e.update('/SYSTEM/TESTS/NATIVE/'+guest,(ROOT/host).read_bytes())
e.update('/SYSTEM/PROGRAMS/LBSTRI.BEX',(ROOT/'build/tests/userland/LBSTRI.BEX').read_bytes())
e.update('/SYSTEM/TESTS/NATIVE/LBSAPPS.BEX',(ROOT/'build/tests/userland/LBSAPPS.BEX').read_bytes())
e.update('/SYSTEM/TESTS/NATIVE/LBSSTART.BEX',(ROOT/'build/tests/userland/LBSSTART.BEX').read_bytes())
e.update('/SYSTEM/TESTS/NATIVE/LBSTEST.BEX',(ROOT/'build/tests/userland/LBSTEST.BEX').read_bytes())
startup=b'4|1|1|10|0|1|60000|LBS test|/SYSTEM/TESTS/NATIVE/LBSTEST.BEX||Native dynamic libraries\n'
if a.triangle:startup=startup.replace(b'/SYSTEM/TESTS/NATIVE/LBSTEST.BEX',b'/SYSTEM/PROGRAMS/LBSTRI.BEX')
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',startup)
e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
sock.unlink(missing_ok=True);serial.unlink(missing_ok=True)
(out/'audio.wav').unlink(missing_ok=True)
cmd=['qemu-system-i386','-accel',a.accel,'-m',str(a.memory),'-smp','2','-vga',a.vga,'-drive',f'file={image},format=raw,if=ide','-netdev','user,id=n0','-device','rtl8139,netdev=n0','-device','AC97,audiodev=lbs-audio','-audiodev',f'wav,id=lbs-audio,path={out / "audio.wav"}','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
if a.cpu:cmd+=['-cpu',a.cpu]
elif a.accel=='kvm':cmd+=['-cpu','host']
if a.virgl:
 cmd[cmd.index('-vga')+1]='none'
 cmd[cmd.index('-display')+1]='egl-headless,rendernode=/dev/dri/renderD128'
 cmd+=['-device','virtio-vga-gl']
proc=subprocess.Popen(cmd,stderr=(out/'qemu.log').open('w'));started=time.monotonic();injected=False;ngs_captured=False;client=None;stream=None;result=None
try:
 def command(payload):
  stream.write((json.dumps(payload)+'\n').encode());stream.flush()
  while True:
   r=json.loads(stream.readline())
   if 'error' in r:raise RuntimeError(r['error'])
   if 'return' in r:return r['return']
 while time.monotonic()-started<a.timeout:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  if proc.poll() is not None:raise RuntimeError(f'QEMU exited {proc.returncode}')
  if '[TASK] excepcion Ring 3' in text:raise RuntimeError(text[-3500:])
  if '[LBSTRI] FAIL' in text:raise RuntimeError(text[-2500:])
  if '[LBSTEST] FAIL' in text:raise RuntimeError(text[text.index('[LBSTEST] FAIL'):][-2500:])
  if a.triangle and '[LBSTRI] FRAME READY' in text and not injected:
   client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(10);client.connect(str(sock));stream=client.makefile('rwb');stream.readline();command({'execute':'qmp_capabilities'})
   time.sleep(.3);command({'execute':'screendump','arguments':{'filename':str((out/'triangle.ppm').resolve())}});injected=True
  if '[LBSTEST] INPUT READY' in text and not injected:
   client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(10);client.connect(str(sock));stream=client.makefile('rwb');stream.readline();command({'execute':'qmp_capabilities'})
   command({'execute':'human-monitor-command','arguments':{'command-line':'mouse_move -2000 -2000'}})
   time.sleep(.1)
   command({'execute':'human-monitor-command','arguments':{'command-line':'mouse_move 120 120'}})
   command({'execute':'human-monitor-command','arguments':{'command-line':'sendkey a'}})
   try:command({'execute':'screendump','arguments':{'filename':str((out/'vision.ppm').resolve())}})
   except RuntimeError:
    if not a.virgl:raise
    (out/'capture-note.txt').write_text('QMP screendump unavailable: EGL headless guest has no CPU scanout surface.\n')
   injected=True
  if '[LBSTEST] NGS FRAME READY' in text and stream and not ngs_captured:
   try:command({'execute':'screendump','arguments':{'filename':str((out/'ngs.ppm').resolve())}})
   except RuntimeError:
    if not a.virgl:raise
   ngs_captured=True
  if ('[LBSTRI] ALL PASS' if a.triangle else '[LBSTEST] ALL PASS') in text:
   assert 'KERNEL PANIC' not in text
   if not a.triangle:
    assert '[LBS:MIGRATION] Network Status uses NETWORK.LBS' in text
    assert '[LBSSTART] ALL PASS' in text
    assert '[LBSAPPS] ALL PASS' in text
   result={'status':'pass','fixture':'triangle' if a.triangle else 'integration','memory_mib':a.memory,'vga':a.vga,'accel':a.accel,'cpus':2,'elapsed_seconds':round(time.monotonic()-started,2),'udp_echo_packets':echo_count[0],'checks':len(re.findall(r'\[LBSTEST\] PASS:',text)),'measurements':re.findall(r'\[LBSTEST\] (?:load_12_ms|private_images_bytes|virtual_images_bytes|file_readonly_bytes|renderer=|reference_100_ms|cold_cycles_5_ms|heap_live_before|input_keys|ngs_fill_100_ms)[^\n]*',text)}
   break
  time.sleep(.2)
 else:raise TimeoutError(text[-4000:])
finally:
 echo_stop.set();echo.close();echo_thread.join(timeout=1)
 if stream:
  try:command({'execute':'quit'})
  except Exception:proc.terminate()
 else:proc.terminate()
 try:proc.wait(timeout=5)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 if stream:stream.close()
 if client:client.close()
 sock.unlink(missing_ok=True)
 if not a.keep_image:image.unlink(missing_ok=True)

if result is not None:
 with wave.open(str(out/'audio.wav'),'rb') as capture:
  frames=capture.getnframes();channels=capture.getnchannels();rate=capture.getframerate();width=capture.getsampwidth()
  assert width==2,'expected signed 16-bit WAV from QEMU'
  samples=array.array('h',capture.readframes(frames));peak=max(abs(s) for s in samples) if samples else 0
  assert frames>0 and peak>0,'emulated audio capture was silent'
  result['audio_capture']={'frames':frames,'channels':channels,'sample_rate':rate,'peak':peak}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
