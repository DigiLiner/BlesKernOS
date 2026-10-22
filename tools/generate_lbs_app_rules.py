#!/usr/bin/env python3
"""Generate native client build prerequisites from the installation manifest."""
import argparse,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',required=True);a=p.parse_args()
apps=json.loads((ROOT/'sdk/lbs/applications.json').read_text())
lines=['# Generated from sdk/lbs/applications.json; do not edit.',
       'LBS_APP_FILES := '+' '.join(v['output'] for v in apps.values()),'']
for key,v in apps.items():
 for path in [key,v['input'],v['output']]:
  if not re.fullmatch(r'[a-zA-Z0-9_/.-]+',path):raise ValueError('invalid make path: '+path)
 lines += [f"{v['output']}: {v['input']} tools/generate_lbs_client.py tools/generate_lbs_host.py sdk/lbs/applications.json $(LBS_CLIENT_CONTRACT) build/sdk/lbs.o $(LBS_POSIX_A) $(SDK_MATH_A) $(LBS_NATIVE_A)",
           f'\t$(PYTHON) tools/generate_lbs_client.py --application {key} --cc $(CC) --ld $(LD)','']
output=Path(a.output);output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(lines)+'\n')
