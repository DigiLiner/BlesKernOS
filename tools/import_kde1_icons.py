#!/usr/bin/env python3
"""Restore the pinned KDE 1 bitmap sources and their upstream notices."""
import io
import json
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'third_party/kde1'
for module in ('kdebase', 'kdelibs'):
    metadata = json.loads((ROOT / (module + '-source.json')).read_text())
    with urllib.request.urlopen(metadata['url'], timeout=60) as response:
        archive = tarfile.open(fileobj=io.BytesIO(response.read()), mode='r:gz')
    with archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            relative = Path(*Path(member.name).parts[1:])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unsafe source archive member')
            notice = any(word in relative.name.lower() for word in
                         ('readme', 'copying', 'license', 'authors', 'copyright', 'credits'))
            if relative.suffix.lower() not in ('.xpm', '.png') and not notice:
                continue
            destination = ROOT / module / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.extractfile(member).read())
    print(f"{module}: {metadata['commit']}")
