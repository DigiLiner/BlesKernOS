#!/usr/bin/env python3
"""Control positivo de las pruebas en Linux; NO certifica ejecucion en BlesK."""
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]
guest=ROOT/'ports/wine/rootfs/WINEGATE'
results={}
for case in [1,2,3,4,5,6,7,13,14,18,19,20,26]:
    process=subprocess.run([str(guest/'lib/ld-linux.so.2'),'--library-path',str(guest/'lib'),str(guest/'bin'/f'case{case:02d}')],
        capture_output=True,text=True,timeout=10)
    results[str(case)]={'exit':process.returncode,'stdout':process.stdout,'stderr':process.stderr}
out=ROOT/'ports/wine/build/host-runtime.json';out.write_text(json.dumps({'host_only':True,'results':results},indent=2)+'\n')
print(out)
if any(r['exit'] for r in results.values()):raise SystemExit(1)
