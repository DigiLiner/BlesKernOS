#!/usr/bin/env python3
"""Package the exact W3 runtime after successful fresh and cold-prefix gates."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('base', 'runtime', 'module', 'run', 'reboot', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    first = read(args.run / 'result.json')
    cold = read(args.reboot / 'result.json')
    if not first.get('passed') or not cold.get('passed'):
        parser.error('fresh and cold-prefix validation must both pass')
    if Path(cold['source']).resolve() != args.run.resolve():
        parser.error('cold-prefix report refers to a different initial run')
    if cold['kernel_sha256'] != sha(args.run / 'kernel-test.bin'):
        parser.error('cold-prefix kernel hash differs')
    staged = read(args.run / 'staged-wine-data.json')
    ntdll_guest = '/opt/wine/lib/wine/i386-unix/ntdll.so'
    if sha(args.module / 'ntdll.so') != staged[ntdll_guest]:
        parser.error('ntdll differs from the validated module')
    dest = args.output.resolve()
    if dest.exists():
        parser.error('use a new output directory')
    root = dest / 'rootfs'
    shutil.copytree(args.base / 'rootfs', root, symlinks=True)
    shutil.copytree(args.base / 'licenses', dest / 'licenses')
    # Match run-runtime.py: runtime root first, package libraries override
    # except libc; the package does not override the dynamic loader.
    libs = root / 'WINEGATE/lib'
    current = args.runtime / 'WINEGATE/lib'
    runtime_hashes = read(args.run / 'staged-files.json')
    for source in current.iterdir():
        if not source.is_file() or source.name == 'libgate.so':
            continue
        target = libs / source.name
        if not target.exists() or source.name in ('libc.so.6', 'ld-linux.so.2'):
            guest = str(source.relative_to(args.runtime))
            if sha(source) != runtime_hashes[guest]:
                raise RuntimeError('runtime manifest mismatch: ' + guest)
            shutil.copyfile(source, target)
    shutil.copytree(args.runtime / 'WINEGATE/etc/fonts',
                    root / 'WINEGATE/etc/fonts', dirs_exist_ok=True)
    for source in (root / 'WINEGATE/etc/fonts').rglob('*'):
        if source.is_file() and sha(source) != runtime_hashes[str(source.relative_to(root))]:
            raise RuntimeError('font configuration differs from the validated run')
    for relative in ('WINEGATE/share/fonts', 'WINEGATE/cache/fontconfig'):
        (root / relative).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.module / 'ntdll.so', root / ntdll_guest.lstrip('/'))
    # Every staged Wine file must match, not merely the replaced module.
    for guest, digest in staged.items():
        target = root / guest.lstrip('/')
        if not target.is_file() or sha(target) != digest:
            raise RuntimeError('validated Wine file mismatch: ' + guest)
    for library in libs.iterdir():
        if not library.is_file():
            continue
        # New runtime libraries retain their distribution copyright notices.
        for directory in ('/usr/lib32', '/usr/lib/i386-linux-gnu', '/lib32',
                          '/lib/i386-linux-gnu'):
            original = Path(directory) / library.name
            if not original.is_file() or sha(original) != sha(library):
                continue
            query = subprocess.run(['dpkg-query', '-S', str(original.resolve())],
                                   text=True, capture_output=True)
            if query.returncode:
                continue
            package = query.stdout.split(': ')[0]
            notice = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
            if notice.is_file():
                shutil.copyfile(notice, dest / 'licenses' /
                                (package.replace(':', '-') + '-copyright'))
            break
    evidence = dest / 'validation'
    evidence.mkdir()
    for name, source in (('fresh', args.run), ('cold', args.reboot)):
        for file in ('result.json', 'command.json', 'serial.log'):
            shutil.copyfile(source / file, evidence / (name + '-' + file))
    shutil.copyfile(args.module / 'manifest.json', dest / 'ntdll-build.json')
    patches = dest / 'patches'
    patches.mkdir()
    for patch in read(args.module / 'manifest.json')['patches']:
        source = Path(patch['file'])
        if sha(source) != patch['sha256']:
            raise RuntimeError('source patch changed since the module build')
        shutil.copyfile(source, patches / source.name)
    shutil.copyfile(args.base / 'manifest.json', dest / 'base-manifest.json')
    manifest = dict(scope='Existing Wine package plus validated ntdll and runtime',
        base_manifest_sha256=sha(args.base / 'manifest.json'),
        kernel_sha256=cold['kernel_sha256'], fresh=first, cold=cold,
        files={str(p.relative_to(root)): sha(p) for p in root.rglob('*')
               if p.is_file() and not p.is_symlink()})
    (dest / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    hashes = {str(p.relative_to(dest)): sha(p) for p in dest.rglob('*')
              if p.is_file() and not p.is_symlink()}
    (dest / 'sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    archive = dest.with_name(dest.name + '.tar.gz')
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(dest, arcname=dest.name)
    archive.with_name(archive.name + '.sha256').write_text(
        sha(archive) + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__':
    main()
