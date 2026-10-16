#!/usr/bin/env python3
"""Check the incremental font install in an actual BFS4 filesystem."""
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import bfs4
from bfs4_update import Editor
with tempfile.TemporaryDirectory(prefix='bles-font-image-') as tmp:
    tmp=Path(tmp);source=tmp/'source';source.mkdir()
    (source/'USER').mkdir();(source/'USER'/'keep.txt').write_bytes(b'user data\n')
    image=tmp/'test.img'
    with image.open('wb') as out:out.truncate(160*1024*1024)
    bfs4.build(image,source,160,'FONT TEST',bfs4.BOOT_SECTORS)
    editor=Editor(image)
    inode=editor.mkdir('/SYSTEM/FONTS')
    assert editor.mkdir('/SYSTEM/FONTS')==inode
    for font in (ROOT/'assets/fonts').glob('*.BFN'):
        editor.update('/SYSTEM/FONTS/'+font.name,font.read_bytes())
    editor.update('/SYSTEM/FONTS/LICENSE.TXT',(ROOT/'assets/fonts/LICENSE.txt').read_bytes())
    assert editor.contents('/USER/keep.txt')==b'user data\n'
    editor.finish();bfs4.check(image)
    editor=Editor(image)
    assert editor.contents('/USER/keep.txt')==b'user data\n'
    for font in (ROOT/'assets/fonts').glob('*.BFN'):
        assert editor.contents('/SYSTEM/FONTS/'+font.name)==font.read_bytes()
    try:editor.mkdir('/USER/keep.txt')
    except ValueError:pass
    else:raise AssertionError('mkdir accepted a regular file')
    editor.fd.close()
print('PASS BFS4 fonts install: directories, exact payloads and existing user data')
