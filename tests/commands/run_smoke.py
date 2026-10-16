#!/usr/bin/env python3
"""Boot real command BEXs; disk mutations use a fresh disposable secondary disk."""
import argparse,http.server,json,re,socket,socketserver,subprocess,sys,threading,time
from pathlib import Path
from fixtures import FTPHandler,payload,received
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,default=Path('/tmp/blesk-command-smoke'));p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img');p.add_argument('--preview',action='store_true');a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
class HTTP(http.server.BaseHTTPRequestHandler):
 def do_HEAD(self):self.send_response(200);self.send_header('Content-Length','5');self.end_headers()
 def do_GET(self):
  self.send_response(302 if self.path=='/redirect' else 200)
  if self.path=='/redirect':self.send_header('Location','/chunked');self.send_header('Content-Length','0');self.end_headers();return
  if self.path=='/chunked':self.send_header('Transfer-Encoding','chunked');self.end_headers();self.wfile.write(b'2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n');return
  data=payload if self.path=='/large' else b'hello';self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 def log_message(self,*args):pass
http=http.server.ThreadingHTTPServer(('0.0.0.0',0),HTTP);ftp=socketserver.ThreadingTCPServer(('0.0.0.0',0),FTPHandler);ftp.daemon_threads=True
for server in [http,ftp]:threading.Thread(target=server.serve_forever,daemon=True).start()
source=(ROOT/'Makefile').read_text();names=source[source.index('COMMAND_NAMES :='):source.index('COMMAND_COMMON_OBJ :=')].split(':=',1)[1].replace('\\','').split()
with (out/'build.log').open('w') as f:subprocess.run(['make','-j2','build/stage2.bin','build/system/services/vision.o','build/system/drivers/NETSTACK.DVR','build/programs/shelllauncher.o']+['build/system/commands/'+n+'.o' for n in names],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,check=True)
subprocess.run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT/'sdk/include'),'-DCMD_HTTP_PORT='+str(http.server_port),'-DCMD_FTP_PORT='+str(ftp.server_address[1]),'-c',str(ROOT/'tests/commands/command_smoke.c'),'-o',str(out/'CMDTEST.BEX')],check=True)
image=out/'system.img';scratch=out/'scratch.img';sock=out/'qmp.sock';sock.unlink(missing_ok=True)
subprocess.run(['cp','--reflink=auto',str(a.base.resolve()),str(image)],check=True)
with image.open('r+b') as disk:
 disk.seek(512);disk.write((ROOT/'build/stage2.bin').read_bytes());disk.seek(9*512);disk.write((ROOT/'build/kernel.bin').read_bytes().ljust(1280*1024,b'\0'))
with scratch.open('wb') as disk:disk.truncate(128*1024*1024)
e=Editor(image)
for n in names:e.update('/SYSTEM/COMMANDS/'+n.upper()+'.BEX',(ROOT/'build/system/commands'/f'{n}.o').read_bytes())
e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/system/services/vision.o').read_bytes());e.update('/SYSTEM/DRIVERS/NETSTACK.DVR',(ROOT/'build/system/drivers/NETSTACK.DVR').read_bytes());e.update('/SYSTEM/PROGRAMS/CMDTEST.BEX',(out/'CMDTEST.BEX').read_bytes())
e.update('/SYSTEM/PROGRAMS/SHELL.BEX',(ROOT/'build/programs/shelllauncher.o').read_bytes())
startup=b'4|1|1|10|0|1|3000|Command test|/SYSTEM/PROGRAMS/CMDTEST.BEX||Command test\n'
if a.preview:startup+=b'5|1|1|11|0|1|12000|Terminal|/SYSTEM/PROGRAMS/SHELL.BEX||Terminal\n'
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',startup);e.finish()
log=out/'guest.log';log.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-smp','1','-m','128','-drive',f'file={image},format=raw,if=ide,index=0','-drive',f'file={scratch},format=raw,if=ide,index=1','-vga','vmware','-display','none','-serial',f'file:{log}','-netdev','user,id=net0','-device','rtl8139,netdev=net0','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown'],stderr=(out/'qemu.log').open('w'))
try:
 deadline=time.monotonic()+180
 while time.monotonic()<deadline:
  text=log.read_text(errors='replace') if log.exists() else ''
  assert '[PANIC]' not in text and '[TASK] excepcion' not in text,text[-3000:]
  if '[CMDTEST] DONE' in text:break
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  time.sleep(.2)
 assert '[CMDTEST] DONE' in text,text[-4000:]
 assert '[CMDTEST] FAIL' not in text,text[-5000:]
 assert re.search(r'\[CMDTEST\] DONE checks=\d+ failures=0',text),text[-2000:]
 assert received.get('uploaded.txt')==b'alpha beta\nGamma\nz',received
 # Final MBR on the exclusively owned scratch disk must have no partitions.
 with scratch.open('rb') as d:mbr=d.read(512)
 assert mbr[510:]==b'\x55\xaa' and mbr[446:510]==bytes(64)
 result={'passed':True,'summary':re.findall(r'\[CMDTEST\] DONE[^\r\n]*',text),'checks':re.findall(r'\[CMDTEST\] PASS[^\r\n]*',text),'ftp_upload_verified':True,'scratch_mbr_verified':True}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
 s=socket.socket(socket.AF_UNIX);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
 def qmp(command,args=None):
  f.write((json.dumps({'execute':command,'arguments':args or {}})+'\n').encode())
  while True:
   message=json.loads(f.readline())
   if 'error' in message:raise RuntimeError(message)
   if 'return' in message:return message['return']
 qmp('qmp_capabilities')
 if a.preview:
  time.sleep(2)
  for char in 'help diskpart':
   qmp('send-key',{'keys':[{'type':'qcode','data':'spc' if char==' ' else char}],'hold-time':15});time.sleep(.08)
  qmp('send-key',{'keys':[{'type':'qcode','data':'ret'}]});time.sleep(2)
  qmp('screendump',{'filename':str(out/'terminal.ppm')})
  from PIL import Image
  Image.open(out/'terminal.ppm').save(out/'terminal.png')
 qmp('quit');proc.wait(timeout=5)
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
 for server in [http,ftp]:server.shutdown();server.server_close()
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True);scratch.unlink(missing_ok=True)
