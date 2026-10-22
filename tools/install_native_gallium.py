#!/usr/bin/env python3
"""Install a verified native Gallium package in an offline BFS4 image copy."""
from pathlib import Path
import argparse,hashlib,json,subprocess
from bfs4_update import Editor
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--validation',type=Path,default=ROOT/'build/native-gallium/validation/result.json')
    p.add_argument('--mesa-validation',type=Path,help='required when the package includes Mesa demos')
    p.add_argument('--require-pacing',action='store_true',help='also require measured compositor pacing gates for both suites')
    a=p.parse_args()
    if a.base.resolve()==a.output.resolve() or a.output.exists():
        p.error('output must be a new image path, different from base')
    report=json.loads(a.validation.read_text())
    if not report.get('pass'):raise ValueError('Native Gallium has not passed functional and performance validation')
    if a.require_pacing and report.get('pacing_pass') is not True:
        raise ValueError('Native Gallium has not passed compositor pacing validation')
    manifest_path=ROOT/'build/native-gallium/manifest.json'
    if hashlib.sha256(manifest_path.read_bytes()).hexdigest()!=report.get('package_manifest_sha256'):
        raise ValueError('Package changed after validation')
    manifest=json.loads(manifest_path.read_text())
    if manifest.get('mesa_demos'):
        if not a.mesa_validation:raise ValueError('Mesa demos require --mesa-validation')
        mesa_report=json.loads(a.mesa_validation.read_text())
        if a.require_pacing and mesa_report.get('pacing_pass') is not True:
            raise ValueError('Mesa demos have not passed compositor pacing validation')
        if (not mesa_report.get('pass') or mesa_report.get('suite')!='mesa-demos' or
            mesa_report.get('package_manifest_sha256')!=report.get('package_manifest_sha256') or
            mesa_report.get('core_sha256')!=report.get('core_sha256')):
            raise ValueError('Mesa demos have not passed on this exact package/core')
    core={name:(ROOT/name).read_bytes() for name in [
        'build/kernel.bin','build/stage2.bin','build/system/drivers/VIRTIOGPU.DVR','build/system/services/vision.o']}
    for name,data in core.items():
        if hashlib.sha256(data).hexdigest()!=report.get('core_sha256',{}).get(name):
            raise ValueError('Core artifact changed after validation: '+name)
    package=ROOT/'build/native-gallium/package'
    for name,digest in manifest['files'].items():
        if hashlib.sha256((package/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('package hash mismatch: '+name)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run(['cp','--reflink=auto',str(a.base),str(a.output)],check=True)
    with a.output.open('r+b') as stream:
        stream.seek(512);stream.write(core['build/stage2.bin'])
        kernel=core['build/kernel.bin']
        if len(kernel)>1280*1024:raise ValueError('kernel reservation exceeded')
        stream.seek(9*512);stream.write(kernel.ljust(1280*1024,b'\0'))
    e=Editor(a.output)
    for name in manifest['files']:
        target='/'+name;e.mkdir(str(Path(target).parent));e.update(target,(package/name).read_bytes())
    e.update('/SYSTEM/DRIVERS/VIRTIOGPU.DVR',core['build/system/drivers/VIRTIOGPU.DVR'])
    e.update('/SYSTEM/SERVICES/VISION.BEX',core['build/system/services/vision.o'])
    e.finish();print(a.output)
if __name__=='__main__':main()
