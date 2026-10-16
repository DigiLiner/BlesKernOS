"""Convert original X11 BDF strikes to BFN2; nearest-neighbour scaling only."""
from pathlib import Path
from PIL import Image
import struct
from font_pack import encode

def read_bdf(path):
    glyphs={};ascent=descent=size=0;current={};bitmap=False
    for line in Path(path).read_text().splitlines():
        parts=line.split()
        if not parts:continue
        key=parts[0]
        if key=='PIXEL_SIZE':size=int(parts[1])
        elif key=='FONT_ASCENT':ascent=int(parts[1])
        elif key=='FONT_DESCENT':descent=int(parts[1])
        elif key=='STARTCHAR':current={};bitmap=False
        elif key=='ENCODING':current['cp']=int(parts[1])
        elif key=='DWIDTH':current['advance']=int(parts[1])
        elif key=='BBX':current['box']=tuple(map(int,parts[1:]))
        elif key=='BITMAP':bitmap=True;current['rows']=[]
        elif key=='ENDCHAR':
            bitmap=False;w,h,left,bottom=current['box'];mask=Image.new('L',(max(1,w),max(1,h)))
            for y,row in enumerate(current['rows']):
                bits=int(row,16);padded=len(row)*4
                for x in range(w):
                    if bits & (1<<(padded-x-1)):mask.putpixel((x,y),255)
            glyphs[current['cp']]=(w,h,left,-bottom-h,current['advance'],mask)
        elif bitmap:current['rows'].append(line.strip())
    return size,ascent,descent,glyphs

def compile_bitmap(paths,name,flags,codepoints):
    strikes=[read_bdf(p) for p in paths];entries=[];blocks=[];offset=64+65*16
    for size in range(8,73):
        # Above native sizes prefer exact 2x/3x pixels over fractional resizing.
        exact=[s for s in strikes if size>s[0] and size%s[0]==0 and size//s[0]<=3]
        if size>max(s[0] for s in strikes) and exact:
            native,ascent,descent,glyphs=max(exact,key=lambda s:s[0])
        else:
            native,ascent,descent,glyphs=min(strikes,key=lambda s:abs(s[0]-size))
        def scale(n):return round(n*size/native)
        records=[];pixels=bytearray();start=4+len(codepoints)*20
        for cp in codepoints:
            w,h,left,top,advance,mask=glyphs.get(cp,glyphs[ord('?')])
            w,h=scale(w),scale(h)
            records.append(struct.pack('<IIHHhhI',cp,start+len(pixels),w,h,scale(left),scale(top),scale(advance)*64))
            if w and h:pixels.extend(mask.resize((w,h),Image.Resampling.NEAREST).tobytes())
        block=encode(struct.pack('<I',len(records))+b''.join(records)+pixels)
        entries.append(struct.pack('<HHhHII',size,scale(ascent)+scale(descent),scale(ascent),0,offset,len(block)))
        blocks.append(block);offset+=len(block)
    return struct.pack('<4sHH48sII',b'BFN1',2,65,name.encode().ljust(48,b'\0'),flags,0)+b''.join(entries)+b''.join(blocks)
