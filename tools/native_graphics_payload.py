"""Native 3D runtime and demos temporarily shipped in normal OS images."""
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def native_graphics_payloads():
    package = ROOT / 'build/native-gallium/package'
    manifest = json.loads((package.parent / 'manifest.json').read_text())
    required = {'SYSTEM/PROGRAMS/GLMARK2.BEX', 'SYSTEM/PROGRAMS/GLMARK2.ELF',
                'SYSTEM/PROGRAMS/MDGEARS.BEX', 'SYSTEM/PROGRAMS/MDGLOSS.BEX',
                'SYSTEM/PROGRAMS/MDMORPH3D.BEX'}
    if not manifest.get('mesa_demos') or not required <= manifest['files'].keys():
        raise ValueError('Native 3D package incomplete; run make mesa-demos-native')
    files = {}
    for name, digest in manifest['files'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid native 3D package path: ' + name)
        source = package / relative
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError('Native 3D package changed; rebuild: ' + name)
        files['/' + name] = source
    files['/SYSTEM/DOCS/3DTESTS.TXT'] = ROOT / 'docs/graphics/NORMAL_3D_TESTS.md'
    files['/SYSTEM/SCREENS/SSJELLY.SCV'] = ROOT / 'build/system/screensavers/ss_jelly.o'
    return files


def stage_native_graphics_payloads(staging, files):
    # glmark2 indexes resources from readdir names using case-sensitive model
    # names and suffixes. Case-insensitive open() alone is not sufficient.
    for guest, source in files.items():
        target = staging / guest.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
