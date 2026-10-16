#!/usr/bin/env python3
"""Application profiles preserve saved preferences and do not limit global GL."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from wine_app_defaults import app_defaults, QUAKE2_EXTENSIONS

fifa = r'[Software\\Wine\\AppDefaults\\fifa2000.exe\\Direct3D]'
quake = r'[Software\\Wine\\AppDefaults\\quake2.exe\\OpenGL]'
unrelated = b'WINE REGISTRY Version 2\n\n[Other]\n"value"="keep"\n'
seeded = app_defaults(unrelated)
assert seeded.startswith(unrelated)
assert (fifa + '\n"csmt"=dword:00000000\n').encode() in seeded
assert (quake + '\n"EnabledExtensions"="' + QUAKE2_EXTENSIONS + '"\n').encode() in seeded
assert app_defaults(seeded) == seeded
assert r'[Software\\Wine\\OpenGL]'.encode() not in seeded
assert len(QUAKE2_EXTENSIONS) < 1024  # ample room for Wine's legacy extensions
for header, name, saved in [(fifa, 'csmt', 'dword:00000001'),
                            (quake, 'EnabledExtensions', '"GL_CUSTOM_saved"')]:
    for spelling in (header, header.upper()):
        section = spelling + ' 12345\n"' + name + '"=' + saved + '\n'
        existing = section.encode() + unrelated
        updated = app_defaults(existing)
        assert updated.startswith(existing)
        assert app_defaults(updated) == updated
        other_section = b'\n[Other]\n"value"="keep"\n'
        absent = (spelling + '\n"OtherPreference"="keep"\n').encode() + other_section
        updated = app_defaults(absent)
        assert updated.count(('"' + name + '"=').encode()) == 1
        assert b'"OtherPreference"="keep"' in updated and other_section in updated
        assert app_defaults(updated) == updated
    assert app_defaults(header.encode()).count(header.encode()) == 1
print('PASS FIFA and Quake II profiles, scope, idempotence, case/EOF, saved values and unrelated settings')
