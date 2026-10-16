#!/usr/bin/env python3
"""Build native-size white Retrosmart CURs and indexed BKCA animations offline."""
import ast, hashlib, json, re, struct
from pathlib import Path
from PIL import ImageColor
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'third_party/retrosmart'
OUTPUT = ROOT/'assets/cursors'
ALIASES = {'default':'ARROW', 'size_hor':'HRESIZE', 'size_ver':'VRESIZE',
           'size_fdiag':'DRESIZE1', 'size_bdiag':'DRESIZE2'}

def bitmap(path):
    strings = [ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', path.read_text())]
    w,h,n,cpp = map(int, strings[0].split())
    assert w == h == 32 and cpp == 1
    palette=[]; lookup={}
    for s in strings[1:n+1]:
        color = re.split(r'\s+c\s+', ' '+s[cpp:], maxsplit=1)[1].strip().lower()
        colors={'none':(0,0,0,0),'cyan':(255,255,255,255),'coral':(0,0,0,255),
                'white':(255,255,255,255),'black':(0,0,0,255)}
        rgba = colors[color] if color in colors else ImageColor.getcolor(color, 'RGBA')
        lookup[s[:cpp]]=len(palette);palette.append(rgba)
    rows=strings[n+1:n+1+h]
    assert len(rows)==h and all(len(r)==w for r in rows)
    return palette, [[lookup[c] for c in row] for row in rows]

def cursor(path,hx,hy):
    palette,rows=bitmap(path)
    assert len(palette)<=16 and 0<=hx<32 and 0<=hy<32
    xor=bytearray();mask=bytearray()
    for row in reversed(rows):
        xor.extend((row[x]<<4)|row[x+1] for x in range(0,32,2))
        for x in range(0,32,8):
            mask.append(sum((not palette[row[x+i]][3])<<(7-i) for i in range(8)))
    colors=b''.join(bytes((b,g,r,0)) for r,g,b,a in palette).ljust(64,b'\0')
    dib=struct.pack('<IiiHHIIiiII',40,32,64,1,4,0,len(xor)+len(mask),0,0,16,0)+colors+xor+mask
    return struct.pack('<HHHBBBBHHII',0,2,1,32,32,16,0,hx,hy,len(dib),22)+dib

def main():
    OUTPUT.mkdir(parents=True,exist_ok=True)
    upstream=json.loads((SOURCE/'UPSTREAM.json').read_text())
    assert hashlib.sha256((SOURCE/'UPSTREAM.TGZ').read_bytes()).hexdigest()==upstream['archive_sha256']
    manifest=[]
    for config in sorted((SOURCE/'src').glob('*.in')):
        frames=[]
        for line in config.read_text().splitlines():
            fields=line.split()
            if not fields or fields[0]!='32':continue
            _,hx,hy,png,*delay=fields
            data=cursor(SOURCE/'src/bitmap.base'/Path(png).with_suffix('.xpm'),int(hx),int(hy))
            frames.append((int(delay[0]) if delay else 0,data))
        if not frames:raise ValueError(config)
        name=ALIASES.get(config.stem,config.stem.upper())
        (OUTPUT/(name+'.CUR')).write_bytes(frames[0][1])
        if len(frames)>1:
            offset=8+12*len(frames);entries=[];payload=[]
            for delay,data in frames:
                assert 0<delay<=10000
                entries.append(struct.pack('<III',delay,offset,len(data)))
                payload.append(data);offset+=len(data)
            (OUTPUT/(name+'.BCA')).write_bytes(b'BKCA'+struct.pack('<HH',1,len(frames))+b''.join(entries+payload))
        manifest.append({'role':config.stem,'file':name+'.CUR','frames':len(frames),'durations_ms':[d for d,_ in frames]})
    (OUTPUT/'INDEX.JSON').write_text(json.dumps({'upstream':upstream,'size':32,'variant':'white, no shadow','cursors':manifest},indent=2)+'\n')
    (OUTPUT/'LICENSE.TXT').write_bytes((SOURCE/'LICENSE').read_bytes())
    (OUTPUT/'SOURCE.TGZ').write_bytes((SOURCE/'UPSTREAM.TGZ').read_bytes())
    (OUTPUT/'README.TXT').write_text('Retrosmart by Manuel Dominguez Lopez, GPL-3.0.\nWhite bitmap variant at native 32x32, no shadow, no scaling.\nUpstream: '+upstream['repository']+'\nCommit: '+upstream['commit']+'\nSOURCE.TGZ contains the unmodified upstream source and notices.\nBKCA v1: magic, u16 version, u16 frame count; count entries of\nu32 delay_ms, u32 absolute offset, u32 CUR size, then CUR payloads.\nConversion source: /SYSTEM/CURSORS/BUILD.PY\n')
    (OUTPUT/'BUILD.PY').write_bytes(Path(__file__).read_bytes())
    print('Built',len(manifest),'Retrosmart roles; animated:',[(m['role'],m['frames']) for m in manifest if m['frames']>1])
if __name__=='__main__':main()
