#!/usr/bin/env python3
"""Exercise BFN2 runs/literals at boundaries and validate the complete catalogue."""
from pathlib import Path
import json,hashlib,random,struct,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from font_pack import encode,decode
rng=random.Random(20261009)
for n in [4,5,127,128,129,255,256,257,4096,8192]:
    for data in [rng.randbytes(n),b'\0'*n,(b'ABC'+b'\xff'*130)*n]:
        assert decode(encode(data))==data
for _ in range(100):
    data=bytes(rng.randrange(8) for _ in range(rng.randrange(4,8192)))
    assert decode(encode(data))==data
for invalid in [b'',b'\4\0\0\0',b'\4\0\0\0\x83',b'\4\0\0\0\x84\0',b'\4\0\0\0\3abc']:
    try:decode(invalid)
    except ValueError:pass
    else:raise AssertionError('accepted malformed BFN2 block')
rows=json.loads((ROOT/'assets/fonts/catalog.json').read_text())['families']
assert len(rows)==24 and len({r['name'] for r in rows})==24
assert {p.name for p in (ROOT/'assets/fonts').glob('*.BFN')}=={r['stem'].upper()+'.BFN' for r in rows}
compressed=raw=0
for face in rows:
    assert hashlib.sha256((ROOT/'assets/fonts'/face['source']).read_bytes()).hexdigest()==face['sha256']
    assert (ROOT/'assets/fonts/licenses'/face['license']).is_file()
    for source,expected in face.get('source_hashes',{}).items():
        assert hashlib.sha256((ROOT/'assets/fonts'/source).read_bytes()).hexdigest()==expected
    data=(ROOT/'assets/fonts'/(face['stem'].upper()+'.BFN')).read_bytes()
    magic,version,count,name,flags,_=struct.unpack_from('<4sHH48sII',data)
    assert magic==b'BFN1' and version==2 and count==65
    assert name.split(b'\0')[0].decode()==face['name'] and flags==face['flags']
    for i in range(count):
        size,line,ascent,_,offset,length=struct.unpack_from('<HHhHII',data,64+16*i)
        assert size==8+i and 0<ascent<=line<=256 and offset+length<=len(data)
        block=decode(data[offset:offset+length]);raw+=len(block);compressed+=length
        assert struct.unpack_from('<I',block)[0]==201
        if face.get('format')=='bdf':
            assert set(block[4+201*20:])<={0,255},face['name']
print(f'PASS BFN2: boundary runs, malformed streams, 24 families x 65 sizes; {compressed:,}/{raw:,} bytes ({100*compressed/raw:.1f}%)')
