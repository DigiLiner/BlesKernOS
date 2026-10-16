"""Convert the vendored KDE 1 bitmap collection to shared BKGP resources."""
from __future__ import annotations

import ast
import hashlib
import gzip
import io
import json
import re
import tarfile
import tempfile
import warnings
from dataclasses import replace
from pathlib import Path

from PIL import Image, ImageColor

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/kde1"


def xpm_image(path: Path) -> Image.Image:
    # Historical XPMs contain tabs, padded headers, C comments and named X11
    # colours which Pillow's limited XPM reader does not handle consistently.
    text = path.read_text(encoding='latin1')
    tokens = re.finditer(r'"(?:[^"\\]|\\.)*"|/\*.*?\*/|//[^\n]*', text, re.S)
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', SyntaxWarning)
        lines = [ast.literal_eval(m[0]) for m in tokens if m[0].startswith('"')]
    width, height, count, cpp = map(int, lines[0].split()[:4])
    palette = {}
    named = json.loads((SOURCE / 'x11-colors.json').read_text())
    for line in lines[1:count + 1]:
        fields = re.split(r'\s+(c|g|g4|m|s)\s+', ' ' + line[cpp:])
        values = dict(zip(fields[1::2], fields[2::2]))
        color = next(values[k].strip() for k in ['c', 'g', 'g4', 'm'] if k in values)
        if color.lower() == 'none':
            rgba = (0, 0, 0, 0)
        elif color.startswith('#'):
            digits = color[1:]; size = len(digits) // 3
            if size not in (1, 2, 3, 4) or len(digits) != size * 3:
                raise ValueError(f'{path}: invalid XPM colour {color}')
            rgba = tuple(round(int(digits[i*size:(i+1)*size], 16) * 255 / (16**size-1)) for i in range(3)) + (255,)
        else:
            key = re.sub(r'\s+', '', color).lower()
            rgb = named.get(key)
            rgba = tuple(rgb) + (255,) if rgb else ImageColor.getcolor(color, 'RGBA')
        palette[line[:cpp]] = rgba
    rows = lines[count+1:count+1+height]
    if len(rows) != height or any(len(row) != width * cpp for row in rows):
        raise ValueError(f'{path}: invalid XPM dimensions')
    pixels = bytes(channel for row in rows for x in range(width)
                   for channel in palette[row[x*cpp:(x+1)*cpp]])
    return Image.frombytes('RGBA', (width, height), pixels)


def resource_name(relative: str) -> str:
    name = 'KDE1/' + str(Path(relative).with_suffix('')).replace('kdebase/', 'base/').replace('kdelibs/', 'libs/')
    # BKGP v1 has a 48-byte name field. Keep an unambiguous stable suffix.
    if len(name.encode('ascii')) >= 48:
        name = name[:35] + '~' + hashlib.sha256(relative.encode()).hexdigest()[:11]
    return name


def source_bundle(output: Path) -> None:
    """Ship preferred bitmap sources and original notices with the package."""
    files = sorted(path for path in SOURCE.rglob('*') if path.is_file())
    files += [ROOT / 'tools' / name for name in
              ('build_graphics_pak.py', 'kde1_graphics.py', 'import_kde1_icons.py')]
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as raw:
        temporary = Path(raw.name)
        try:
            with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode='w') as archive:
                    for path in files:
                        data = path.read_bytes()
                        info = tarfile.TarInfo(path.relative_to(ROOT).as_posix())
                        info.size = len(data)
                        info.mode = 0o644
                        archive.addfile(info, io.BytesIO(data))
            raw.close()
            temporary.chmod(0o644)
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)


def candidates(candidate_type):
    resources = []
    by_path = {}
    for path in sorted(SOURCE.rglob('*')):
        if path.suffix.lower() not in ('.xpm', '.png'):
            continue
        relative = path.relative_to(SOURCE).as_posix()
        if relative.startswith('kdebase/compat/'):
            continue  # Chromium/Qt Creator additions from the modern port.
        if relative in {
            'kdebase/kappfinder/pics/xftp.xpm',  # Desktop entry, not a bitmap.
            'kdebase/kfm/pics/info.xpm',  # Undefined pixel colour in upstream.
            'kdebase/kwm/pics/menu.xpm',  # Truncated rows in upstream.
        }:
            continue
        image = xpm_image(path) if path.suffix.lower() == '.xpm' else Image.open(path).convert('RGBA')
        output = io.BytesIO()
        image.save(output, format='PNG', optimize=False, compress_level=9)
        item = candidate_type(resource_name(relative), image.width, image.height, 32, relative, output.getvalue())
        resources.append(item)
        by_path[relative] = item
    aliases = json.loads((SOURCE / 'aliases.json').read_text())
    for name, relative in sorted(aliases.items()):
        resources.append(replace(by_path[relative], name=name))
        item = by_path[relative]
        image = Image.open(io.BytesIO(item.data)).convert('RGBA')
        bounds = image.getbbox()
        if bounds and max(bounds[2]-bounds[0], bounds[3]-bounds[1]) <= 32:
            # Remove transparent margins only; never resize the drawing.
            image = image.crop(bounds)
            canvas = Image.new('RGBA', (32, 32))
            canvas.paste(image, ((32-image.width)//2, (32-image.height)//2))
            encoded = io.BytesIO(); canvas.save(encoded, format='PNG', compress_level=9)
            resources.append(replace(item, name='32/'+name, width=32, height=32, data=encoded.getvalue()))
    small = json.loads((SOURCE / 'small-aliases.json').read_text())
    for name, relative in sorted(small.items()):
        item = by_path[relative]
        if item.width > 16 or item.height > 16:
            raise ValueError(f'{name}: button icon must be a native mini bitmap')
        image = Image.open(io.BytesIO(item.data)).convert('RGBA')
        canvas = Image.new('RGBA', (16, 16))
        canvas.paste(image, ((16-image.width)//2, (16-image.height)//2))
        encoded = io.BytesIO(); canvas.save(encoded, format='PNG', compress_level=9)
        resources.append(replace(item, name='16/'+name, width=16, height=16, data=encoded.getvalue()))
    names = [item.name.lower() for item in resources]
    if len(names) != len(set(names)) or any(len(name.encode('ascii')) >= 48 for name in names):
        raise ValueError('Duplicate or oversized KDE resource names')
    if len(resources) > 4096:
        raise ValueError('KDE resources exceed kernel BKGP entry limit')
    return sorted(resources, key=lambda item: item.name.lower())
