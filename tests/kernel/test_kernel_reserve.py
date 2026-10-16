#!/usr/bin/env python3
"""Comprueba los rechazos reales del linker y NASM; no altera artefactos normales."""
import json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def run(cmd):return subprocess.run([str(x) for x in cmd],cwd=ROOT,text=True,capture_output=True)
results={}
with tempfile.TemporaryDirectory(prefix='blesk-layout-') as tmp:
 d=Path(tmp)
 for name,rodata,bss,ok in [('beyond_old_limit',1100*1024,4096,True),('file_overflow',1280*1024,0,False),('bss_within_reserve',16,0x280000,True),('bss_overflow',16,0x500000,False)]:
  (d/'entry.S').write_text('.section .boot.text,"ax"\n.global _start\n_start: nop\n.section .rodata\n.space '+str(rodata)+',0x5a\n.section .bss\n.space '+str(bss)+'\n')
  a=run(['as','--32',d/'entry.S','-o',d/'entry.o']);assert a.returncode==0,a.stderr
  r=run(['ld','-m','elf_i386','-T','kernel/arch/x86/linker.ld',d/'entry.o','-o',d/'kernel.elf'])
  results[name]={'passed':(r.returncode==0)==ok,'expected_success':ok,'stderr':r.stderr}
 for sectors,ok in [(2560,True),(2561,False)]:
  r=run(['nasm','-f','bin',f'-DKERNEL_SECTORS={sectors}','boot/stage2.asm','-o',d/'stage2.bin'])
  results['stage2_'+str(sectors)]={'passed':(r.returncode==0)==ok,'stderr':r.stderr}
print(json.dumps(results,indent=2))
assert all(x['passed'] for x in results.values())
