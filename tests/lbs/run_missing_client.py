#!/usr/bin/env python3
"""A migrated Editor must fail cleanly when PRINT is absent, without static fallback."""
import json,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
out=ROOT/'tests/lbs/evidence/migration/missing-print';out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
image=ROOT/'build/lbs-missing-print.img';serial=out/'serial.log'
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-lbs-user.img'),str(image)],check=True)
probe=out/'probe.c'
probe.write_text('#include <bleskernos.h>\n#include <bleskernos_api.h>\nvoid bleskernos_program_main(void *arg){(void)arg;int pid=bk_spawn("/SYSTEM/PROGRAMS/TEXTEDITOR.BEX",0);int status=-1;if(pid<0 || bk_waitpid(pid,&status)!=pid || status!=127){bk_sys_log("[LBSMISSING] FAIL exit status\\n");bk_exit(1);}bk_sys_log("[LBSMISSING] PASS exit status 127\\n");bk_exit(0); }\n')
subprocess.run(['gcc','-m32','-ffreestanding','-fno-builtin','-nostdinc','-nostdlib','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT/'sdk/include'),'-c',str(probe),'-o',str(out/'probe.raw.o')],check=True)
subprocess.run(['ld','-m','elf_i386','-r',str(out/'probe.raw.o'),str(ROOT/'build/sdk/libblesk.a'),'-o',str(out/'probe.o')],check=True)
e=Editor(image);e.remove('/SYSTEM/PRINT.LBS');e.mkdir('/SYSTEM/TESTS/NATIVE');e.update('/SYSTEM/TESTS/NATIVE/LBSMISS.BEX',(out/'probe.o').read_bytes());e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Missing PRINT|/SYSTEM/TESTS/NATIVE/LBSMISS.BEX||Missing library test\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
serial.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-m','64','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-no-reboot'],stderr=(out/'qemu.log').open('w'))
try:
 start=time.monotonic()
 while time.monotonic()-start<60:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  assert 'excepcion Ring 3' not in text and 'KERNEL PANIC' not in text
  marker='[LBS:APP] FAIL /SYSTEM/PROGRAMS/TEXTEDITOR.BEX: PRINT.LBS\n'
  assert '[LBSMISSING] FAIL' not in text
  if marker in text and '[LBSMISSING] PASS exit status 127' in text:
   assert '[LBS:APP] /SYSTEM/PROGRAMS/TEXTEDITOR.BEX modules=' not in text
   result={'status':'pass','missing':'/SYSTEM/PRINT.LBS','application':'TEXTEDITOR.BEX','exit_status':127,'scope':'explicit startup error, no application entry/fallback, waitpid confirms termination after partial module load'}
   (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS: missing PRINT rejects migrated Editor startup; exit status 127 observed');break
  time.sleep(.2)
 else:raise TimeoutError(text[-3500:])
finally:
 proc.terminate()
 try:proc.wait(timeout=10)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 image.unlink(missing_ok=True)
