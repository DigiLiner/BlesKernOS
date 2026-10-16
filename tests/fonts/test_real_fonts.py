#!/usr/bin/env python3
"""Exercise the actual renderer with host VFS adapters and memory sanitizers."""
from pathlib import Path
import argparse
import shutil
import sys
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--preview',type=Path)
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='bles-fonts-') as temp:
    temp=Path(temp);fonts=temp/'fonts';fonts.mkdir()
    for source in (ROOT/'assets/fonts').glob('*.BFN'):
        shutil.copyfile(source,fonts/source.name)
    # Keep one actual BFN1 file alongside 23 BFN2 files to verify old packs still work.
    sys.path.insert(0,str(ROOT/'tools'))
    from font_pack import decode
    pack=fonts/'DEJAVUSANS.BFN';data=pack.read_bytes()
    header=list(struct.unpack_from('<4sHH48sII',data));header[1]=1
    entries=[];blocks=[];offset=64+16*header[2]
    for i in range(header[2]):
        size,line,ascent,reserved,start,length=struct.unpack_from('<HHhHII',data,64+i*16)
        block=decode(data[start:start+length]);blocks.append(block)
        entries.append(struct.pack('<HHhHII',size,line,ascent,reserved,offset,len(block)))
        offset+=len(block)
    pack.write_bytes(struct.pack('<4sHH48sII',*header)+b''.join(entries)+b''.join(blocks))
    source=(ROOT/'gui/real_font.c').read_text()
    source='\n'.join(line for line in source.splitlines() if not line.startswith('#include '))
    harness=(ROOT/'tests/fonts/runtime_harness.c').read_text()+source+(ROOT/'tests/fonts/runtime_checks.inc').read_text()
    (temp/'test.c').write_text(harness)
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-iquote',str(ROOT/'sdk/include'),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test'),str(fonts),str(temp/'preview.ppm')],check=True)
    if a.preview:
        from PIL import Image
        a.preview.parent.mkdir(parents=True,exist_ok=True)
        Image.open(temp/'preview.ppm').save(a.preview)
