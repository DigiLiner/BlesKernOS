#!/usr/bin/env python3
"""Build a checked Phase 3/4 image, preserving every unrelated inode and byte."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
import bfs4
from bfs4_update import Editor

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = {
    '/SYSTEM/SERVICES/VISION.BEX': 'build/system/services/vision.o',
    '/SYSTEM/DRIVERS/PS2MOUSE.DVR': 'build/system/drivers/PS2MOUSE.DVR',
    '/SYSTEM/CONTROL/DISPLAY.CPL': 'build/system/control/display.o',
    '/SYSTEM/CONTROL/MOUSE.CPL': 'build/system/control/mouse.o',
    '/SYSTEM/PROGRAMS/FILE.BEX': 'build/programs/filebrowser.o',
    '/SYSTEM/PROGRAMS/TEXTEDITOR.BEX': 'build/programs/texteditor.o',
    '/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX': 'build/programs/processmanager.o',
    '/SYSTEM/SCREENS/SSBALLS.SCV': 'build/system/screensavers/ss_balls.o',
    '/SYSTEM/SCREENS/SSPIPES.SCV': 'build/system/screensavers/ss_pipes.o',
    '/SYSTEM/SDK/INCLUDE/bleskernos_api.h': 'sdk/include/bleskernos_api.h',
}

def offline(path):
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if 'qemu-system' not in (proc/'comm').read_text():
                continue
            for fd in (proc/'fd').iterdir():
                if fd.resolve() == path:
                    raise ValueError('QEMU has this image open: '+str(path))
        except (OSError, PermissionError):
            continue

def inventory(editor):
    result = {}
    for number, inode in editor.inodes.items():
        digest = hashlib.sha256()
        remaining = inode[2]
        for start, count in editor.extents(inode):
            editor.fd.seek(editor.base+start*bfs4.BLOCK)
            extent_bytes = min(remaining, count*bfs4.BLOCK)
            while extent_bytes:
                chunk = editor.fd.read(min(extent_bytes, 1024*1024))
                if not chunk:
                    raise ValueError('truncated file in inode '+str(number))
                digest.update(chunk)
                remaining -= len(chunk)
                extent_bytes -= len(chunk)
        if remaining:
            raise ValueError('incomplete extents in inode '+str(number))
        raw = bytes(editor.table[number*256:(number+1)*256])
        result[number] = (raw, digest.hexdigest())
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or output.exists():
        parser.error('output must be a new image, distinct from source')
    offline(source)
    bfs4.check(source)
    # Snapshot all build inputs before changing any image.
    payload = {guest: (ROOT/host).read_bytes() for guest, host in PAYLOAD.items()}
    stage = (ROOT/'build/stage2.bin').read_bytes()
    kernel = (ROOT/'build/kernel.bin').read_bytes()
    if len(stage) != 4096 or len(kernel) > 1280*1024:
        raise ValueError('boot payload exceeds reserved space')
    before = Editor(source)
    try:
        targets = {before.find(guest) for guest in payload}
        baseline = inventory(before)
    finally:
        before.fd.close()
    temporary = Path(str(output)+'.update')
    if temporary.exists():
        parser.error('temporary output already exists')
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '--reflink=auto', '--preserve=all', str(source), str(temporary)], check=True)
    try:
        editor = Editor(temporary)
        try:
            for guest, data in payload.items():
                editor.update(guest, data)
            editor.finish()
        finally:
            editor.fd.close()
        with temporary.open('r+b') as disk:
            disk.seek(512)
            disk.write(stage)
            disk.seek(9*512)
            disk.write(kernel.ljust(1280*1024, b'\0'))
            disk.flush()
            os.fsync(disk.fileno())
        bfs4.check(temporary)
        after = Editor(temporary)
        try:
            updated = inventory(after)
            if baseline.keys() != updated.keys():
                raise ValueError('inode set changed unexpectedly')
            for number in baseline.keys()-targets:
                if baseline[number] != updated[number]:
                    raise ValueError('unrelated inode changed: '+str(number))
            for guest, data in payload.items():
                if after.contents(guest) != data:
                    raise ValueError('payload mismatch: '+guest)
        finally:
            after.fd.close()
        report = {'source':str(source), 'output':str(output),
                  'preserved_inodes':len(baseline)-len(targets),
                  'kernel_bytes':len(kernel),
                  'kernel_sha256':hashlib.sha256(kernel).hexdigest(),
                  'stage2_sha256':hashlib.sha256(stage).hexdigest(),
                  'sha256':{guest:hashlib.sha256(data).hexdigest() for guest,data in payload.items()}}
        os.replace(temporary, output)
        output.with_suffix('.audit.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2))
    finally:
        temporary.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
