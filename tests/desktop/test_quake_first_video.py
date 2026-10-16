#!/usr/bin/env python3
"""Test the launcher's actual first-session video detector without game assets."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'ports/tinyx/wine-launch.c').read_text()
start = source.index('static int quake2_first_video_setup(')
end = source.index('\nint main(', start)
code = '#include <stdio.h>\n#include <string.h>\n#include <strings.h>\n#include <errno.h>\n#include <sys/stat.h>\n'
code += source[start:end].replace('static int quake2_first_video_setup', 'int quake2_first_video_setup', 1)
with tempfile.TemporaryDirectory(prefix='bk-quake-video-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(code)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', str(work/'test.c'), '-o', str(work/'test.so')], check=True)
    detect = ctypes.CDLL(str(work/'test.so')).quake2_first_video_setup
    detect.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    detect.restype = ctypes.c_int
    game = work/'game'
    (game/'baseq2').mkdir(parents=True)
    path = str(game/'QUAKE2.EXE').encode()
    folder = str(game).encode()
    assert not detect(path, folder)
    pak = game/'baseq2/pak0.pak'
    pak.write_bytes(b'PACK')
    assert detect(path, folder)
    assert detect(str(game/'quake2.exe').encode(), folder)
    assert not detect(str(game/'OTHER.EXE').encode(), folder)
    assert not detect(path, None)
    for name in ['config.cfg', 'autoexec.cfg']:
        config = game/'baseq2'/name
        config.write_text('set vid_fullscreen 1\n')
        before = config.read_bytes()
        assert not detect(path, folder)
        assert config.read_bytes() == before
        config.unlink()
    pak.write_bytes(b'BAD!')
    assert not detect(path, folder)
    pak.write_bytes(b'PA')
    assert not detect(path, folder)
    assert not detect(path, b'/' + b'x'*4100)
print('PASS: first Quake II session detected; saved preferences, other programs and invalid archives preserved')
