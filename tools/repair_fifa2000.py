#!/usr/bin/env python3
"""Repair an installed FIFA 2000 from the owner's ISO in a stopped BFS4 image.

Restore Joliet names without reinstalling or removing aliases. Optionally use
this disc's alternate executable; the SafeDisc original is backed up unchanged.
No game payload is distributed with this tool.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import struct

import bfs4
from bfs4_update import Editor

GAME = '/SYSTEM/WINE/PROGRAM FILES/EA SPORTS/FIFA 2000'
ORIGINAL = 'bfdef2befa0a6b745bab7aede65609784d34dedef0ed223e6b2822928e9afd24'
ALTERNATE = '4e7274613e1c54d60938a27240b39d7d81ae6ffcf5c4b1d9d05c7a1ec1b4dde6'


def iso_tree(path, joliet):
    """Index files by shared data extent, linking primary and Joliet names."""
    with path.open('rb') as stream:
        roots = []
        for lba in range(16, 80):
            stream.seek(lba * 2048)
            sector = stream.read(2048)
            if len(sector) != 2048 or sector[1:6] != b'CD001' or sector[0] == 255:
                break
            if ((not joliet and sector[0] == 1) or
                (joliet and sector[0] == 2 and sector[88:91] in
                 (b'%/@', b'%/C', b'%/E'))):
                roots.append(struct.unpack_from('<I', sector, 158)[0])
        if not roots:
            raise ValueError('ISO lacks the requested directory tree')
        stream.seek(roots[-1] * 2048)
        root = stream.read(34)
        result = {}
        seen = set()

        def walk(extent, size, parent):
            if extent in seen or size > 1024 * 1024:
                raise ValueError('invalid ISO directory tree')
            seen.add(extent)
            stream.seek(extent * 2048)
            data = stream.read(size)
            position = 0
            while position < len(data):
                length = data[position]
                if not length:
                    position = (position // 2048 + 1) * 2048
                    continue
                record = data[position:position + length]
                if length < 34 or len(record) != length or 33 + record[32] > length:
                    raise ValueError('invalid ISO directory record')
                position += length
                raw = record[33:33 + record[32]]
                if raw in (b'\0', b'\1'):
                    continue
                name = raw.decode('utf-16-be' if joliet else 'ascii').split(';')[0].rstrip('.')
                if not name or name in ('.', '..') or '/' in name or '\\' in name:
                    raise ValueError('invalid ISO name')
                extent = struct.unpack_from('<I', record, 2)[0]
                size = struct.unpack_from('<I', record, 10)[0]
                target = parent + '/' + name
                if record[25] & 2:
                    walk(extent, size, target)
                else:
                    result[(extent, size)] = target
        walk(roots[-1], struct.unpack_from('<I', root, 10)[0], '')
        return result


def installed_path(path):
    lower = path.lower()
    if lower.startswith('/data/'):
        return GAME + path
    for prefix in ('/setup/thrash/', '/setup/3dsetup/'):
        if lower.startswith(prefix):
            return GAME + path[len('/setup'):]
    return None


def registry_defaults(text, drive, renderer=None):
    section = r'[Software\\EA SPORTS\\FIFA 2000]'
    start = text.find(section)
    if start < 0:
        text += '\n' + section + ' 0\n'
        start = text.index(section)
    end = text.find('\n[', start + len(section))
    if end < 0:
        end = len(text)
    block = text[start:end]
    for name, value in [('Language', 'english'), ('CD Drive', drive + ':\\'),
                        ('Thrash Driver', 'softdraw')]:
        if not re.search(r'^"' + re.escape(name) + r'"=', block, re.M):
            block += '\n"' + name + '"="' + value.replace('\\', '\\\\') + '"\n'
    if renderer is not None:
        block = re.sub(r'^"Thrash Driver"=.*$', '"Thrash Driver"="'+renderer+'"', block, flags=re.M)
    return text[:start] + block + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--use-disc-executable', action='store_true')
    parser.add_argument('--restore-missing-executable', action='store_true',
                        help='restore the verified original EXE from this ISO if installation omitted it')
    parser.add_argument('--renderer', choices=['dx6','softdraw'],
                        help='explicitly select the game renderer; other settings are preserved')
    args = parser.parse_args()
    image = args.image.resolve()
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if not any(name in (proc / 'comm').read_text() for name in ('qemu-system', 'vmware-vmx')):
                continue
            if any(fd.resolve() == image for fd in (proc / 'fd').iterdir()):
                parser.error('close the VM using this disk first')
        except (OSError, PermissionError):
            pass
    primary = iso_tree(args.iso, False)
    joliet = iso_tree(args.iso, True)
    bfs4.check(image)
    temporary = image.with_suffix(image.suffix + '.fifa-repair')
    if temporary.exists():
        parser.error('temporary repair already exists')
    subprocess.run(['cp', '--reflink=auto', '--', str(image), str(temporary)], check=True)
    try:
        editor = Editor(temporary)
        editor.find(GAME)  # Never create a replacement installation.
        repaired = 0
        for identity, long_name in joliet.items():
            short_name = primary.get(identity)
            if not short_name or short_name.lower() == long_name.lower():
                continue
            source, target = installed_path(short_name), installed_path(long_name)
            if not source or not target:
                continue
            try:
                editor.find(target)
                continue  # Preserve existing long-name files.
            except FileNotFoundError:
                pass
            try:
                data = editor.contents(source)
            except FileNotFoundError:
                # Minimal installs deliberately leave most assets on the CD.
                continue
            if len(data) != identity[1]:
                raise ValueError('installed file size differs: ' + source)
            with args.iso.open('rb') as disc:
                disc.seek(identity[0] * 2048)
                if disc.read(identity[1]) != data:
                    raise ValueError('installed file differs from ISO: ' + source)
            editor.update(target, data)
            repaired += 1
        executable = GAME + '/fifa2000.exe'
        if args.restore_missing_executable or args.use_disc_executable:
            try:
                editor.find(executable)
            except FileNotFoundError:
                original_key = next(k for k,v in primary.items() if v.lower() == '/fifa2000.exe')
                with args.iso.open('rb') as disc:
                    disc.seek(original_key[0]*2048)
                    original = disc.read(original_key[1])
                if hashlib.sha256(original).hexdigest() != ORIGINAL:
                    raise ValueError('unknown original executable on this ISO')
                editor.update(executable, original)
                print('Restored missing original executable from the verified ISO.')
        if args.use_disc_executable:
            executable = GAME + '/fifa2000.exe'
            original = editor.contents(executable)
            digest = hashlib.sha256(original).hexdigest()
            if digest not in (ORIGINAL, ALTERNATE):
                raise ValueError('unknown installed executable; refusing replacement')
            alternate_key = next(k for k, v in primary.items() if v.lower() == '/crack/fifa2000.exe')
            with args.iso.open('rb') as disc:
                disc.seek(alternate_key[0] * 2048)
                alternate = disc.read(alternate_key[1])
            if hashlib.sha256(alternate).hexdigest() != ALTERNATE:
                raise ValueError('unknown alternate executable on this ISO')
            backup = executable + '.safedisc-backup'
            if digest == ORIGINAL:
                try:
                    saved = editor.contents(backup)
                    if saved != original:
                        raise ValueError('existing backup differs from the original')
                except FileNotFoundError:
                    editor.update(backup, original)
                editor.update(executable, alternate)
        registry = '/WINEGATE/prefix/system.reg'
        original_registry = editor.contents(registry)
        backup = registry + '.fifa2000-backup'
        try:
            editor.find(backup)
        except FileNotFoundError:
            editor.update(backup, original_registry)
        drive = None
        for letter in 'defghijklmnopqrstuvwxy':
            try:
                if editor.contents('/WINEGATE/prefix/dosdevices/' + letter + ':') == b'/CDROM':
                    drive = letter.upper()
                    break
            except FileNotFoundError:
                pass
        if drive is None:
            raise ValueError('no configured CD-ROM drive in this Wine prefix')
        text = registry_defaults(original_registry.decode('utf-8'), drive, args.renderer)
        if text.encode() != original_registry:
            editor.update(registry, text.encode())
        editor.finish()
        bfs4.check(temporary)
        os.replace(temporary, image)
        print(f'Repaired {repaired} Joliet names; installed data and aliases preserved.')
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
