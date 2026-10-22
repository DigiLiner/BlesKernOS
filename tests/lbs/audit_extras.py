#!/usr/bin/env python3
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
allowed=set(re.findall(r'(?:EXPORT|ELF_WEAK_EXPORT)\((\w+)\)',(ROOT/'kernel/exec/elf_loader.c').read_text()))
rows=[]
for name,c in json.loads((ROOT/'sdk/lbs/extra_applications.json').read_text()).items():
 p=ROOT/'build/lbs/extras'/name
 if not p.exists():continue
 report=json.loads(p.with_suffix('.json').read_text())
 undef={l.split()[-1] for l in subprocess.check_output(['nm','-u',str(p)],text=True).splitlines() if l.strip()}
 defined={l.split()[-1] for l in subprocess.check_output(['nm','--defined-only',str(p)],text=True).splitlines() if l.strip()}
 assert undef<=allowed,(name,sorted(undef-allowed))
 assert 'bk_lbs_application_main' in defined
 if name=='HYPERZIP.O':
  assert report['imports']['hzip_archive_open']=='ARCHIVE'
  assert 'hz_inflate_raw' not in defined and 'hzip_archive_open' not in defined
 if name in ['3DPMESA.O','3DPTINY.O']:
  assert 'OSMesaCreateContextExt' not in defined and 'tglBegin' not in defined
 rows.append({'name':name,'modules':report['modules'],'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
out=ROOT/'tests/lbs/evidence/total';out.mkdir(parents=True,exist_ok=True)
(out/'extras.json').write_text(json.dumps(rows,indent=2)+'\n')
print('PASS:',len(rows),'optional native clients; public native imports and extracted ZIP/3D')
