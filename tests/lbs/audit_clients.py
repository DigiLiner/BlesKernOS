#!/usr/bin/env python3
"""Audit the installed migration payloads, including original call-site rewrites."""
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def nm(path,*flags):
 return {line.split()[-1] for line in subprocess.check_output(['nm',*flags,str(path)],text=True).splitlines() if line.strip()}
allowed=set(re.findall(r'(?:EXPORT|ELF_WEAK_EXPORT)\((\w+)\)',(ROOT/'kernel/exec/elf_loader.c').read_text()))
rows=[]
for key,config in json.loads((ROOT/'sdk/lbs/applications.json').read_text()).items():
 source=ROOT/config['input'];output=ROOT/config['output'];report=json.loads(output.with_suffix('.json').read_text())
 original=nm(source,'--undefined-only');undefined=nm(output,'--undefined-only');defined=nm(output,'--defined-only')
 assert 'bk_lbs_application_main' in defined and 'bleskernos_program_main' in defined,output
 for symbol in report['imports']:
  assert symbol in original,(output,symbol,'not an original import')
  assert '__lbs_call_'+symbol in defined,(output,symbol,'missing dynamic dispatch')
 assert not any(n.startswith('__lbs_call_') for n in undefined),output
 assert 'bk_file_size' not in defined and 'bk_file_write' not in defined,output
 assert undefined<=allowed,(output,sorted(undefined-allowed))
 if '/appearance.' in key or '/display.' in key:
  assert 'NGS3D' in report['modules']
  assert 'OSMesaCreateContextExt' not in defined,output
 if '/texteditor.' in key:assert 'PRINT' in report['modules'] and 'print_begin' not in defined
 if key.endswith('/viewer.o'):
  assert 'MEDIA' in report['modules'] and 'VISION' in report['modules']
  assert not any(n.startswith(('vv_', 'vr_', 'plm_')) for n in defined),output
  assert 'bk_media_video_open_path' in report['imports']
 if config.get('kind')=='command':
  assert not any(n.startswith('command_') for n in defined),output
 if key.endswith('/imageviewer.o'):
  assert 'bk_ngs_image_blit' in report['imports'] and 'bk_ngs_checker' in report['imports']
 if key.endswith('/archive_manager.o'):
  assert 'ARCHIVE' in report['modules']
 if config.get('kind')=='screensaver' and not key.endswith('/ss_logo.o'):
  assert 'NGS3D' in report['modules'] and 'OSMesaCreateContextExt' not in defined
 rows.append({'destination':config['destination'],'modules':report['modules'],'imports':len(report['imports']),
              'original_bytes':((ROOT/key).stat().st_size if (ROOT/key).exists() else source.stat().st_size),'input_bytes':source.stat().st_size,'migrated_bytes':output.stat().st_size,
              'sha256':hashlib.sha256(output.read_bytes()).hexdigest()})
folder=ROOT/'tests/lbs/evidence/migration';folder.mkdir(parents=True,exist_ok=True)
(folder/'clients.json').write_text(json.dumps(rows,indent=2)+'\n')
print('PASS:',len(rows),'native application/CPL clients; call sites, native imports, shared PRINT/Mesa and entrypoints')
