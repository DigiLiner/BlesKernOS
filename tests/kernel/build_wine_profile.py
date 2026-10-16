#!/usr/bin/env python3
"""Build an isolated W3 profiler without replacing production objects."""
from pathlib import Path
import argparse,re,shlex,subprocess
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
lines=subprocess.check_output(['make','-Bn','build/kernel.elf'],cwd=root,text=True).splitlines()
commands=[shlex.split(line) for line in lines if line.startswith(('gcc ','ld '))]
replacements={}
for name in ['kernel/api/syscall.c','kernel/drivers/core/vfs.c','kernel/drivers/core/block.c','kernel/drivers/storage/ahci.c']:
 old='build/'+name[:-2]+'.o';obj=out/(Path(name).stem+'.o')
 cmd=next(c.copy() for c in commands if name in c and '-c' in c);cmd[cmd.index('-o')+1]=str(obj);cmd.insert(1,'-DW3_PROFILE=1')
 subprocess.run(cmd,cwd=root,check=True);replacements[old]=str(obj)
# Extra scopes only in these copies. Relative includes keep their original meaning.
for name,scopes in {
 'kernel/mm/memory.c':{'bool mm_vm_frame_alloc(':'535','static heap_block_t *mm_find_block_locked(':'536','uint32_t mm_user_heap_available(':'537','bool mm_user_heap_pin(':'538'},
 'kernel/mm/pager.c':{'bool pager_pin(':'539'},
}.items():
 src=root/name;s=src.read_text()
 s=re.sub(r'^(#include )"([^"]+)"',lambda m:m[1]+'"'+str((src.parent/m[2]).resolve())+'"',s,flags=re.M)
 s='#include "'+str(root/'kernel/include/w3_profile.h')+'"\n'+s
 for signature,scope in scopes.items():
  start=s.index(signature);brace=s.index('{',start)+1;s=s[:brace]+'\n    W3P_SCOPE('+scope+',0U);'+s[brace:]
 copy=out/Path(name).name;copy.write_text(s);obj=copy.with_suffix('.o');old='build/'+name[:-2]+'.o'
 cmd=next(c.copy() for c in commands if name in c and '-c' in c)
 cmd[cmd.index('-c')+1]=str(copy);cmd[cmd.index('-o')+1]=str(obj);cmd.insert(1,'-DW3_PROFILE=1')
 subprocess.run(cmd,cwd=root,check=True);replacements[old]=str(obj)
profiler=root/'ports/wine/tests/w3-profile.c';s=profiler.read_text()
s=re.sub(r'^(#include )"([^"]+)"',lambda m:m[1]+'"'+str((profiler.parent/m[2]).resolve())+'"',s,flags=re.M)
s=s.replace('static bool w3p_enabled;','static bool w3p_enabled=true;\nstatic volatile uint64_t w3p_last_clock;')
s=s.replace('uint64_t elapsed = w3p_clock() - started;','uint64_t now=w3p_clock();w3p_last_clock=now;\n    uint64_t elapsed = now - started;')
copy=out/'w3-profile.c';copy.write_text(s);obj=copy.with_suffix('.o')
cmd=next(c.copy() for c in commands if 'kernel/api/syscall.c' in c and '-c' in c)
cmd[cmd.index('-c')+1]=str(copy);cmd[cmd.index('-o')+1]=str(obj);cmd.insert(1,'-DW3_PROFILE=1');subprocess.run(cmd,cwd=root,check=True)
cmd=next(c.copy() for c in commands if c[0]=='ld' and 'build/kernel.elf' in c)
cmd=[replacements.get(token,token) for token in cmd];cmd[cmd.index('-o')+1]=str(out/'kernel.elf');cmd.insert(cmd.index('-o'),str(obj));subprocess.run(cmd,cwd=root,check=True)
subprocess.run(['objcopy','-O','binary',str(out/'kernel.elf'),str(out/'kernel.bin')],check=True)
kernel=(out/'kernel.bin').read_bytes()
subprocess.run(['nasm','-f','bin',f'-DKERNEL_SECTORS={(len(kernel)+511)//512}',f'-DKERNEL_CHECKSUM={sum(kernel)&0xffffffff}','boot/stage2.asm','-o',str(out/'stage2.bin')],cwd=root,check=True)
print('Diagnostic kernel:',out)
