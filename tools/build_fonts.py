#!/usr/bin/env python3
"""Compile redistributable TrueType fonts into bounded, integer-only BFN1 packs.
Normal OS builds use the checked-in packs; regeneration requires Pillow/FreeType.
"""
import argparse
import json
from concurrent.futures import ProcessPoolExecutor
from font_pack import encode
from pathlib import Path
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'assets/fonts/catalog.json'
FAMILIES = json.loads(CATALOG.read_text())['families']
FACES = [(f['stem'], f['name'], f['flags'], f) for f in FAMILIES]
CODEPOINTS = list(range(32, 127)) + list(range(160, 256)) + [0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x20ac, 0xfffd]
MIN_SIZE, MAX_SIZE = 8, 72
HEADER = struct.Struct('<4sHH48sII')
STRIKE = struct.Struct('<HHhHII')
GLYPH = struct.Struct('<IIHHhhI')

def compile_font(source, name, flags):
    blocks, entries = [], []
    offset = HEADER.size + STRIKE.size * (MAX_SIZE - MIN_SIZE + 1)
    for size in range(MIN_SIZE, MAX_SIZE + 1):
        font = ImageFont.truetype(str(source), size, layout_engine=ImageFont.Layout.BASIC)
        # Variable fonts default to differing weights; use their Regular instance.
        try:
            axes=font.get_variation_axes()
        except OSError:
            axes=[]
        if axes:
            values=[]
            for axis in axes:
                label=axis['name'].decode() if isinstance(axis['name'],bytes) else axis['name']
                desired=400 if label=='Weight' else (14 if label=='Optical Size' else axis['default'])
                values.append(max(axis['minimum'],min(axis['maximum'],desired)))
            font.set_variation_by_axes(values)
        ascent, descent = font.getmetrics()
        records, masks = [], bytearray()
        data_start = 4 + len(CODEPOINTS) * GLYPH.size
        for cp in CODEPOINTS:
            char = chr(cp)
            left, top, right, bottom = font.getbbox(char, anchor='ls')
            width, height = right - left, bottom - top
            mask = Image.new('L', (max(1, width), max(1, height)))
            ImageDraw.Draw(mask).text((-left, -top), char, font=font, fill=255, anchor='ls')
            records.append(GLYPH.pack(cp, data_start + len(masks), width, height, left, top, round(font.getlength(char)*64)))
            if width and height:
                masks.extend(mask.tobytes())
        block = struct.pack('<I', len(records)) + b''.join(records) + masks
        block = encode(block)
        entries.append(STRIKE.pack(size, ascent + descent, ascent, 0, offset, len(block)))
        blocks.append(block)
        offset += len(block)
    return HEADER.pack(b'BFN1', 2, len(entries), name.encode().ljust(48, b'\0'), flags, 0) + b''.join(entries) + b''.join(blocks)

def build_face(args):
    output, stem, name, flags, face = args
    if face.get('format') == 'bdf':
        from bitmap_fonts import compile_bitmap
        data=compile_bitmap([ROOT/'assets/fonts'/p for p in face['strikes']],name,flags,CODEPOINTS)
    else:
        data=compile_font(ROOT/'assets/fonts'/face['source'],name,flags)
    target=output/(stem.upper()+'.BFN')
    temporary=target.with_suffix('.tmp')
    temporary.write_bytes(data);temporary.replace(target)
    return f'{target.name}: {len(data)} bytes; {MIN_SIZE}..{MAX_SIZE} px'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'assets/fonts')
    parser.add_argument('--family', action='append', help='regenerate selected family stems only')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    selected=[face for face in FACES if not args.family or face[0] in args.family]
    if not selected:parser.error('no matching families')
    with ProcessPoolExecutor(max_workers=max(1,args.jobs)) as pool:
        for message in pool.map(build_face,[(args.output,*face) for face in selected]):
            print(message,flush=True)
    families=json.loads(CATALOG.read_text())['families']
    text='BlesKernOS font collection — original fonts and rasterized BFN packs\n\n'
    for face in families:
        text+=f"{face['name']}: {face['upstream']}\nLicense: {face['license']}\n\n"
    for license_name in sorted({f['license'] for f in families}):
        text+='\n'+'='*72+'\n'+license_name+'\n'+'='*72+'\n'
        text+=(ROOT/'assets/fonts/licenses'/license_name).read_text()+'\n'
    (args.output/'LICENSE.txt').write_text(text)

if __name__ == '__main__':
    main()
