"""Bounded PackBits-style byte runs used by BFN2 (no external runtime codec)."""
import struct

def encode(data):
    out=bytearray(struct.pack('<I',len(data)))
    i=0
    while i<len(data):
        run=1
        while run<128 and i+run<len(data) and data[i+run]==data[i]:run+=1
        if run>=3:
            out.extend((0x80|(run-1),data[i]));i+=run
            continue
        begin=i;i+=run
        while i<len(data) and i-begin<128:
            run=1
            while run<3 and i+run<len(data) and data[i+run]==data[i]:run+=1
            if run>=3:break
            i+=min(run,128-(i-begin))
        out.append(i-begin-1);out.extend(data[begin:i])
    return bytes(out)

def decode(data,limit=2*1024*1024):
    if len(data)<4:raise ValueError('truncated BFN2 block')
    expected=struct.unpack_from('<I',data)[0]
    if expected<4 or expected>limit:raise ValueError('invalid decoded block size')
    out=bytearray();i=4
    while i<len(data):
        tag=data[i];i+=1;n=(tag&127)+1
        if n>expected-len(out):raise ValueError('decoded block overflow')
        if tag&128:
            if i==len(data):raise ValueError('truncated run')
            out.extend(bytes([data[i]])*n);i+=1
        else:
            if n>len(data)-i:raise ValueError('truncated literal')
            out.extend(data[i:i+n]);i+=n
    if len(out)!=expected:raise ValueError('short decoded block')
    return bytes(out)
