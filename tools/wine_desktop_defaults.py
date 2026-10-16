"""Vision consumes Windows associations, not Linux XDG MIME exports."""
import re


def desktop_defaults(data: bytes) -> bytes:
    text = data.decode('utf-8')
    header = r'[Software\\Wine\\FileOpenAssociations]'
    match = re.search(r'^' + re.escape(header) + r'[^\n]*(?:\n|$)', text, re.M)
    if not match:
        return (text.rstrip() + '\n\n' + header + '\n"Enable"="N"\n').encode('utf-8')
    end = text.find('\n[', match.end())
    if end < 0:
        end = len(text)
    section = text[match.end():end]
    if re.search(r'^"Enable"=', section, re.M):
        section = re.sub(r'^"Enable"=.*$', '"Enable"="N"', section, flags=re.M)
    else:
        section = section.rstrip() + '\n"Enable"="N"\n'
    return (text[:match.end()] + section + text[end:]).encode('utf-8')
