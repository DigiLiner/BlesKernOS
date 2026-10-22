#!/usr/bin/env python3
"""Compile production Mesa sources with GCC stack-usage diagnostics."""
import json,shlex,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];result=[]
with tempfile.TemporaryDirectory(prefix='bk-mesa-stack-') as td:
 for name in ['s_triangle','s_texture','s_aatriangle','s_span']:
  source=f'libs/Mesa-3.5/src/swrast/{name}.c';target=f'build/sdk/lbs/pic/{source[:-2]}.o'
  plan=subprocess.check_output(['make','-n','-B',target],cwd=ROOT,text=True)
  command=next(shlex.split(line) for line in plan.splitlines() if line.startswith('gcc ') and f'-c {source} ' in line)
  output=Path(td)/(name+'.o');command[command.index('-o')+1]=str(output);command+=['-fstack-usage']
  subprocess.run(command,cwd=ROOT,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  for line in output.with_suffix('.su').read_text().splitlines():
   fields=line.split('\t');function=fields[0].split(':')[-1];size=int(fields[1])
   result.append({'function':function,'bytes':size,'source':source})
report={'max_bytes':max(r['bytes'] for r in result),'functions_over_16k':[r for r in result if r['bytes']>16384]}
out=ROOT/'tests/lbs/evidence/mesa-stack.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));assert report['max_bytes']<=32768,'software rasterizer frame exceeds native stack budget'
