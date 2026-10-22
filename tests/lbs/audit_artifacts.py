#!/usr/bin/env python3
"""Validate the actual artifacts, dependency closure and native imports."""
from pathlib import Path
import json,re,subprocess,struct
ROOT=Path(__file__).resolve().parents[2];folder=ROOT/'build/system/libs/native'
expected=json.loads((ROOT/'sdk/lbs/dependencies.json').read_text())
def output(*args):return subprocess.check_output(args,text=True)
libs={};all_exports=set();rows=[]
for name,deps in expected.items():
 p=folder/(name+'.LBS');raw=p.read_bytes()
 assert raw[:7]==b'\x7fELF\x01\x01\x01' and struct.unpack_from('<HH',raw,16)==(3,3),p
 dynamic=output('readelf','-d',str(p));needed=re.findall(r'\(NEEDED\).*?\[(.*?)\]',dynamic)
 assert set(needed)=={d+'.LBS' for d in deps},(name,needed,deps)
 assert '(TEXTREL)' not in dynamic and '(HASH)' in dynamic,name
 exported={line.split()[-1] for line in output('nm','-D','--defined-only',str(p)).splitlines() if line.strip()}
 undefined={line.split()[-1] for line in output('nm','-D','--undefined-only',str(p)).splitlines() if line.strip()}
 assert 'bk_lbs_info' in exported and f'bk_{name.lower()}_abi_1' in exported,name
 assert not any('resource_acquire' in n or 'resource_release' in n or n.endswith('_inner') for n in exported),name
 all_exports|=exported;libs[name]={'exports':exported,'imports':undefined,'deps':deps}
 rows.append({'library':name+'.LBS','bytes':len(raw),'exports':len(exported)})
def closure(name,stack=()):
 assert name not in stack,stack+(name,)
 result={name}
 for dep in libs[name]['deps']:result|=closure(dep,stack+(name,))
 return result
for name,lib in libs.items():
 scope=set().union(*(libs[n]['exports'] for n in closure(name)))
 for symbol in lib['imports']:
  if symbol in all_exports:assert symbol in scope,(name,'missing DT_NEEDED for',symbol)
# The final ET_REL must only leave imports understood by the existing native
# kernel loader; SDK runtime functions must have been linked into the program.
registry=(ROOT/'kernel/exec/elf_loader.c').read_text()
allowed=set(re.findall(r'(?:EXPORT|ELF_WEAK_EXPORT)\((\w+)\)',registry))
for name in ['LBSTEST.BEX','LBSSTART.BEX']:
 for line in output('nm','--undefined-only',str(ROOT/'build/tests/userland'/name)).splitlines():
  parts=line.split()
  if len(parts)==2 and parts[0]=='U':assert parts[1] in allowed,(name,'unavailable native import',parts[1])
report={'libraries':rows,'total_bytes':sum(r['bytes'] for r in rows),'status':'pass'}
p=ROOT/'tests/lbs/evidence/artifacts.json';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS: {len(rows)} ELF32 ET_DYN artifacts, ABI markers, acyclic dependency closure, hidden internals and native imports; {report["total_bytes"]} file bytes')
