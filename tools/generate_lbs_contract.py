#!/usr/bin/env python3
"""Update client prerequisites only when exported/imported ABI names change.

Implementation-only LBS updates are installed without relinking clients. Native
runtime archives and client sources remain explicit prerequisites of each client.
"""
import argparse,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',required=True);a=p.parse_args()
rows={}
for name in json.loads((ROOT/'sdk/lbs/dependencies.json').read_text()):
 lib=ROOT/'build/system/libs/native'/(name+'.LBS')
 def names(flag):
  return sorted(l.split()[-1] for l in subprocess.check_output(['nm','-D',flag,str(lib)],text=True).splitlines() if l.strip())
 dynamic=subprocess.check_output(['readelf','-d',str(lib)],text=True)
 rows[name]={'exports':names('--defined-only'),'imports':names('--undefined-only'),
             'needed':sorted(re.findall(r'\(NEEDED\).*?\[(.*?)\]',dynamic))}
output=Path(a.output);output.parent.mkdir(parents=True,exist_ok=True)
content=json.dumps(rows,sort_keys=True,indent=2)+'\n'
if not output.exists() or output.read_text()!=content:output.write_text(content)
