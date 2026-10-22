#!/usr/bin/env python3
"""Run shared command parsing, ARCHIVE and descriptor-domain I/O in the real OS."""
import argparse,json,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from lbs_payload import application_payloads
p=argparse.ArgumentParser();p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-lbs-user.img');a=p.parse_args()
out=ROOT/'tests/lbs/evidence/total/commands';out.mkdir(parents=True,exist_ok=True)
subprocess.run(['gcc','-m32','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT/'sdk/include'),'-c',str(ROOT/'tests/lbs/commands_probe.c'),'-o',str(out/'raw.o')],check=True)
subprocess.run(['ld','-m','elf_i386','-r',str(out/'raw.o'),str(ROOT/'build/sdk/libblesk_lbs_posix.a'),str(ROOT/'build/sdk/libblesk_lbs_native.a'),'-o',str(out/'probe.o')],check=True)
image=ROOT/'build/lbs-commands.img';serial=out/'serial.log'
subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
e=Editor(image)
payloads=application_payloads()
for name in ['compress','extract','copy','echo','checksum']:
 guest='/SYSTEM/COMMANDS/'+name+'.BEX'
 assert e.contents(guest)==payloads[guest].read_bytes(),guest
for host in (ROOT/'build/system/libs/native').glob('*.LBS'):
 assert e.contents('/SYSTEM/'+host.name)==host.read_bytes(),host.name
e.remove('/TEMP/LBSCMD.OK');e.mkdir('/SYSTEM/TESTS/NATIVE');e.update('/SYSTEM/TESTS/NATIVE/COMMANDS.BEX',(out/'probe.o').read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|60000|Commands|/SYSTEM/TESTS/NATIVE/COMMANDS.BEX||Shared command test\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish();e.fd.close()
serial.unlink(missing_ok=True);(out/'result.json').unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-m','256','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-no-reboot'],stderr=(out/'qemu.log').open('w'))
try:
 start=time.monotonic()
 while time.monotonic()-start<180:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  if any(s in text for s in ['[LBS:COMMANDS] FAIL','[LBS:APP] FAIL','excepcion Ring 3','free invalido','KERNEL PANIC']):raise RuntimeError(text[-4000:])
  if '[LBS:COMMANDS] ALL PASS' in text:
   (out/'result.json').write_text(json.dumps({'status':'pass','commands':['compress','extract','copy','echo','checksum'],'round_trip':True},indent=2)+'\n');print('PASS: dynamic commands, shared ARCHIVE, parser, copy and exact file bytes');break
  time.sleep(.2)
 else:raise TimeoutError(text[-4000:])
finally:
 proc.terminate()
 try:proc.wait(timeout=5)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 if (out/'result.json').exists():
  checked=Editor(image)
  try: assert checked.contents('/TEMP/LBSCMD.OK')==b'PASS'
  except BaseException:
   (out/'result.json').unlink(missing_ok=True);raise
  finally: checked.fd.close()
 image.unlink(missing_ok=True)
