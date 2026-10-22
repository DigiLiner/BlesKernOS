#!/usr/bin/env python3
"""Attest unchanged tested graphics payload after adding passive SDK/notices.

Never turns a failed execution into a pass. Checks every existing package file
against the stopped execution image; only new source/relink and notice files
may be added. Runtime, programs, resources, SDK headers and core must match.
"""
from pathlib import Path
import argparse,hashlib,json
from bfs4_update import Editor
ROOT=Path(__file__).resolve().parents[1]
def digest(data):return hashlib.sha256(data).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--validation',type=Path,required=True)
    p.add_argument('--tested-image',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists():p.error('use a new attestation output path')
    original_bytes=a.validation.read_bytes();report=json.loads(original_bytes)
    if not report.get('pass'):raise ValueError('execution did not pass')
    manifest_path=ROOT/'build/native-gallium/manifest.json';manifest=json.loads(manifest_path.read_text())
    for name,expected in report['core_sha256'].items():
        if digest((ROOT/name).read_bytes())!=expected:raise ValueError('core changed: '+name)
    image=Editor(a.tested_image);existing={};added={}
    try:
        for name,expected in manifest['files'].items():
            current=(ROOT/'build/native-gallium/package'/name).read_bytes()
            if digest(current)!=expected:raise ValueError('manifest mismatch: '+name)
            try:tested=image.contents('/'+name)
            except FileNotFoundError:
                if not name.startswith(('SYSTEM/SDK/MESADEMOS/','SYSTEM/SHARE/LICENSES/')):
                    raise ValueError('new executable/data payload requires a new execution: '+name)
                added[name]=expected;continue
            if tested!=current:raise ValueError('tested payload changed; repeat execution: '+name)
            existing[name]=expected
    finally:image.fd.close()
    if not existing:raise ValueError('no common tested payload')
    report['tested_package_manifest_sha256']=report['package_manifest_sha256']
    report['package_manifest_sha256']=digest(manifest_path.read_bytes())
    report['payload_equivalence']={
        'execution_repeated':False,'source_report':str(a.validation.resolve()),
        'source_report_sha256':digest(original_bytes),'tested_image':str(a.tested_image.resolve()),
        'unchanged_files':existing,'passive_additions':added,'pass':True}
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {len(existing)} tested files and core unchanged; {len(added)} passive additions')
if __name__=='__main__':main()
