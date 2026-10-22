#!/usr/bin/env python3
"""Opt-in native host imports; all addresses are ET_REL Ring 3 trampolines."""
from pathlib import Path
import subprocess,argparse
p=argparse.ArgumentParser();p.add_argument("--modules",nargs="*");p.add_argument("--output",default="build/sdk/lbs_host.c");args=p.parse_args()
root=Path(__file__).resolve().parents[1]
all_files={p.name:p for p in (root/'build/system/libs/native').glob('*.LBS')}
selected=set((n if n.endswith('.LBS') else n+'.LBS') for n in args.modules) if args.modules else set(all_files)
pending=list(selected)
import re
while pending:
 n=pending.pop()
 for dep in re.findall(r'\(NEEDED\).*?\[(.*?)\]',subprocess.check_output(['readelf','-d',str(all_files[n])],text=True)):
  if dep not in selected:selected.add(dep);pending.append(dep)
files=[all_files[n] for n in sorted(selected)]
imports=set()
for f in files:
 for line in subprocess.check_output(['nm','-D','--undefined-only',str(f)],text=True).splitlines():
  if line.strip():imports.add(line.split()[-1])
exports=set()
for f in files:
 for line in subprocess.check_output(['nm','-D','--defined-only',str(f)],text=True).splitlines():
  if line.strip():exports.add(line.split()[-1])
# The native loader supplies each original symbol via its checked ABI. Runtime
# libc imports use the process's static POSIX runtime before reaching this hook.
lines=['#include <string.h>']
for i,n in enumerate(sorted(imports-exports)):
 target=n.removeprefix('__lbs_host_')
 lines.append(f'extern char lbs_import_{i}[] __asm__("{target}");')
lines.append('void *bk_lbs_host_symbol(const char *name){')
for i,n in enumerate(sorted(imports-exports)):
 lines.append(f' if(!strcmp(name,"{n}"))return (void*)lbs_import_{i};')
lines+=[' return 0;','}']
(root/args.output).write_text('\n'.join(lines)+'\n')
print(f'LBS host: {len(imports-exports)} imports')
