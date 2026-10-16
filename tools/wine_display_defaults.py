"""Display defaults for TinyX's fixed physical framebuffer.

Wine's real mode emulation handles logical resolution/depth and scales the
client surface; the X server does not claim a physical mode change occurred.
Preserve explicit user preferences and every unrelated registry section.
"""
import re

def display_defaults(data: bytes) -> bytes:
    text = data.decode('utf-8')
    header = r'[Software\\Wine\\X11 Driver]'
    start = text.find(header)
    if start < 0:
        return (text.rstrip()+'\n\n'+header+'\n"EmulateModeset"="Y"\n').encode('utf-8')
    end = text.find('\n[', start+len(header))
    if end < 0: end = len(text)
    section = text[start:end]
    if re.search(r'^"EmulateModeset"=', section, re.M): return data
    return (text[:end].rstrip()+'\n"EmulateModeset"="Y"\n\n'+text[end:]).encode('utf-8')
