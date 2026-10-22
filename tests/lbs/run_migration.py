#!/usr/bin/env python3
"""Boot installed LBS applications/CPLs and verify actual dynamic startup/lifetime.

Wine launchers are short-lived forwarders, checked separately through Wine's
window test. Desktop programs must resolve their LBS imports, remain alive and
survive process teardown; each case gets a QMP screenshot for UI review.
"""
import argparse,hashlib,json,re,socket,subprocess,sys,time,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from lbs_payload import application_payloads,APPLICATIONS
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-lbs-user.img')
p.add_argument('--memory',type=int,default=256);p.add_argument('--vga',default='std')
p.add_argument('--accel',choices=['tcg','kvm'],default='tcg');p.add_argument('--virgl',action='store_true')
p.add_argument('--tag',default='')
p.add_argument('--extras',action='store_true')
p.add_argument('--data-image',type=Path,help='optional supplemental game data disk')
p.add_argument('--only',nargs='+',help='destination basenames to exercise')
p.add_argument('--repeat',type=int,default=1)
p.add_argument('--diagnose-stall',action='store_true')
p.add_argument('--settle-ms',type=int,default=4000)
p.add_argument('--timeout',type=int,default=400);a=p.parse_args()
if a.settle_ms<500:p.error('--settle-ms must be at least 500')
label=('virgl' if a.virgl else a.vga)+f'-{a.memory}m-{a.accel}'+(f'-settle{a.settle_ms}' if a.settle_ms!=4000 else '')+('-'+a.tag if a.tag else '')
out=ROOT/'tests/lbs/evidence/migration'/label;out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
paths=[v['destination'] for v in APPLICATIONS.values() if v.get('kind','gui') == 'gui']
extra_files={};launch_args={}
if a.extras:
 entries=json.loads((ROOT/'sdk/lbs/extra_applications.json').read_text())
 extra_files={'/SYSTEM/PROGRAMS/'+name:ROOT/'build/lbs/extras'/name for name in entries}
 launch_args={'/SYSTEM/PROGRAMS/'+name:c.get('launch_args') for name,c in entries.items()}
 paths=['/SYSTEM/PROGRAMS/'+name for name,c in entries.items() if not c.get('forwarder') and not c.get('headless')]
 label+='-extras'
 out=ROOT/'tests/lbs/evidence/migration'/label;out.mkdir(parents=True,exist_ok=True)
 (out/'result.json').unlink(missing_ok=True)

if a.only:
 paths=[p for p in paths if Path(p).name.upper() in {n.upper() for n in a.only}]
 if not paths:raise ValueError('No matching applications')

if a.repeat<1:raise ValueError('repeat must be positive')
paths*=a.repeat

source='''#include <bleskernos.h>
#include <bleskernos_api.h>
#include <bleskernos_admin.h>
static void report(const char *prefix,const char *path){bk_sys_log(prefix);bk_sys_log(path);bk_sys_log("\\n");}
void bleskernos_program_main(void *unused){(void)unused;
const char *paths[]={PATHS};
const char *arguments[]={ARGUMENTS};
for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
 report("[LBSMIG] starting ",paths[i]);int pid=bk_spawn(paths[i],arguments[i]);
 if(pid<0){report("[LBSMIG] FAIL spawn ",paths[i]);bk_exit(1);}
 bk_sys_sleep_ms(4000);
 bk_process_info_t tasks[128];unsigned count=bk_proc_snapshot(tasks,128);int alive=0;
 for(unsigned j=0;j<count;j++)if(tasks[j].process_id==(unsigned)pid && tasks[j].state!=BK_PROCESS_ZOMBIE && tasks[j].state!=BK_PROCESS_UNUSED)alive=1;
 if(!alive){report("[LBSMIG] FAIL lifetime ",paths[i]);bk_exit(2);}
 report("[LBSMIG] PASS startup ",paths[i]);bk_sys_sleep_ms(700);
 if(!bk_proc_end_process(pid)){report("[LBSMIG] FAIL cleanup ",paths[i]);bk_exit(3);}
 int status;
 for(unsigned attempt=0;attempt<100;attempt++){
  int live=0;count=bk_proc_snapshot(tasks,128);
  for(unsigned j=0;j<count;j++)if(tasks[j].process_id==(unsigned)pid && tasks[j].state!=BK_PROCESS_ZOMBIE && tasks[j].state!=BK_PROCESS_UNUSED)live=1;
  if(!live)break;
  if(attempt==99){report("[LBSMIG] FAIL surviving thread ",paths[i]);bk_exit(4);}
  bk_sys_sleep_ms(100);
 }
 bk_waitpid(pid,&status);
 report("[LBSMIG] PASS cleanup ",paths[i]);
}bk_sys_log("[LBSMIG] ALL PASS\\n");bk_exit(0);}
'''.replace('PATHS',','.join(json.dumps(path) for path in paths)).replace('ARGUMENTS',','.join(json.dumps(launch_args[p]) if launch_args.get(p) else '0' for p in paths)).replace('bk_sys_sleep_ms(4000)',f'bk_sys_sleep_ms({a.settle_ms})')
probe=out/'probe.c';probe.write_text(source)
subprocess.run(['gcc','-m32','-ffreestanding','-fno-builtin','-nostdinc','-nostdlib','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT/'sdk/include'),'-c',str(probe),'-o',str(out/'probe.raw.o')],check=True)
subprocess.run(['ld','-m','elf_i386','-r',str(out/'probe.raw.o'),str(ROOT/'build/sdk/libblesk.a'),'-o',str(out/'probe.o')],check=True)
image=ROOT/f'build/lbs-migration-{label}.img'
qmp_dir=tempfile.TemporaryDirectory(prefix='bk-lbs-mig-');sock=Path(qmp_dir.name)/'qmp.sock'
(out/'qmp-path.txt').write_text(str(sock))
subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
e=Editor(image);hashes={}
for guest,host in application_payloads().items():
 assert e.contents(guest)==host.read_bytes(),guest
 hashes[guest]=hashlib.sha256(host.read_bytes()).hexdigest()
for lib in (ROOT/'build/system/libs/native').glob('*.LBS'):
 assert e.contents('/SYSTEM/'+lib.name)==lib.read_bytes(),lib.name
 hashes['/SYSTEM/'+lib.name]=hashlib.sha256(lib.read_bytes()).hexdigest()
 try:e.find('/SYSTEM/LIBS/NATIVE/'+lib.name)
 except FileNotFoundError:pass
 else:raise AssertionError('duplicate library in former directory: '+lib.name)
for guest,host in extra_files.items():
 data=host.read_bytes();e.update(guest,data);assert e.contents(guest)==data,guest
 hashes[guest]=hashlib.sha256(data).hexdigest()
if a.extras:
 for guest,host in [('/DOOM/DOOM1.WAD',ROOT/'extras/Doom-BlesKernOS-Standalone/DOOM1.WAD'),('/QUAKE/ID1/PAK0.PAK',ROOT/'extras/Quake-BlesKernOS-Standalone/pak0.pak')]:
  e.mkdir(str(Path(guest).parent));e.update(guest,host.read_bytes())
e.mkdir('/SYSTEM/TESTS/NATIVE');e.update('/SYSTEM/TESTS/NATIVE/LBSMIG.BEX',(out/'probe.o').read_bytes())
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|60000|Migration test|/SYSTEM/TESTS/NATIVE/LBSMIG.BEX||LBS application lifecycle\n')
e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\r\n')
e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
(out/'inputs.json').write_text(json.dumps(hashes,indent=2)+'\n')
sock.unlink(missing_ok=True);serial=out/'serial.log';serial.unlink(missing_ok=True)
cmd=['qemu-system-i386','-accel',a.accel,'-m',str(a.memory),'-smp','2','-cpu','host' if a.accel=='kvm' else 'qemu32','-vga',a.vga,'-drive',f'file={image},format=raw,if=ide','-netdev','user,id=n0','-device','rtl8139,netdev=n0','-device','AC97,audiodev=silent','-audiodev','none,id=silent','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
if a.data_image:cmd+=['-drive',f'file={a.data_image.resolve()},format=raw,if=ide,index=1,snapshot=on']
if a.virgl:
 cmd[cmd.index('-vga')+1]='none';cmd[cmd.index('-display')+1]='egl-headless,rendernode=/dev/dri/renderD128';cmd+=['-device','virtio-vga-gl']
if a.diagnose_stall:cmd+=['-gdb','tcp:127.0.0.1:12369']
proc=subprocess.Popen(cmd,stderr=(out/'qemu.log').open('w'));start=time.monotonic();client=None;stream=None;captured=set();last_progress=time.monotonic();diagnosed=False
def command(name,args=None):
 stream.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode());stream.flush()
 while True:
  response=json.loads(stream.readline())
  if 'error' in response:raise RuntimeError(response['error'])
  if 'return' in response:return response['return']
try:
 while time.monotonic()-start<a.timeout:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  failure=any(s in text for s in ['[LBSMIG] FAIL','[LBS:APP] FAIL','excepcion Ring 3','KERNEL PANIC','free invalido'])
  if a.diagnose_stall and not diagnosed and captured and (failure or time.monotonic()-last_progress>25):
   import run_qemu_desktop_diagnostic as diagnostic
   class Connection:
    def call(self,name,arguments=None):return command(name,arguments)
   try:
    diagnostic.capture(Connection(),12369,out)
   finally:command('cont')
   diagnosed=True
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  if 'free invalido' in text and '\n' not in text[text.rfind('free invalido'):]:
   time.sleep(.05);continue
  if any(s in text for s in ['[LBSMIG] FAIL','[LBS:APP] FAIL','excepcion Ring 3','KERNEL PANIC','free invalido']):raise RuntimeError(text[-5000:])
  for path in dict.fromkeys(paths):
   if '[LBSMIG] PASS startup '+path not in text:continue
   if path in captured:continue
   if '[LBS:APP] '+path+' modules=' not in text:continue
   if stream is None:
    client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(15);client.connect(str(sock));stream=client.makefile('rwb');stream.readline();command('qmp_capabilities')
   if not a.virgl:command('screendump',{'filename':str((out/(Path(path).name+'.ppm')).resolve())})
   captured.add(path);last_progress=time.monotonic();print('PASS dynamic startup:',path,flush=True)
  if '[LBSMIG] ALL PASS' in text:
   assert set(paths)==captured,(set(paths)-captured)
   assert len(re.findall(r'\[LBSMIG\] PASS cleanup ',text))==len(paths)
   result={'status':'pass','memory_mib':a.memory,'vga':'virgl' if a.virgl else a.vga,'accel':a.accel,'applications':paths,'elapsed_seconds':round(time.monotonic()-start,2),'settle_ms':a.settle_ms,'scope':'resolved LBS imports, running process after settling interval, no live process threads after group exit; screenshots for UI review'}
   (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');break
  time.sleep(.15)
 else:raise TimeoutError(text[-5000:])
finally:
 if stream:
  try:command('quit')
  except Exception:proc.terminate()
 else:proc.terminate()
 try:proc.wait(timeout=10)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 if stream:stream.close()
 if client:client.close()
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
