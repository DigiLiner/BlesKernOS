#!/usr/bin/env python3
"""Verify the launcher's locale using Wine's actual libc-C fallback parser.
Usage: python3 ports/wine/tests/test_launcher_locale.py /path/to/wine-source
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root=pathlib.Path(__file__).resolve().parents[3]
launcher=(root/'ports/tinyx/wine-launch.c').read_text()
locale=re.search(r'"LC_ALL=([^"\n]+)"',launcher)
assert locale, 'Launcher must supply Wine a concrete locale in the archive-free runtime'
source=(pathlib.Path(sys.argv[1])/'dlls/ntdll/unix/env.c').read_text()
start=source.index('static BOOL unix_to_win_locale(')
end=source.index('\n\nstatic const NLS_LOCALE_DATA *get_win_locale',start)
function=source[start:end]
program='''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define BOOL int
#define TRUE 1
#define FALSE 0
#define LOCALE_NAME_MAX_LENGTH 85
'''+function+'''
int main(int argc, char **argv) {
    char system[85] = "", user[85] = "";
    /* libc C is what an unavailable locale archive leaves behind. */
    unsetenv("LC_ALL");
    assert(!unix_to_win_locale("C",system));
    assert(!system[0]); /* empty name would resolve to invariant in locale.nls */
    setenv("LC_ALL",argv[1],1);
    assert(unix_to_win_locale("C",system));
    assert(unix_to_win_locale("C",user));
    assert(!strcmp(system,"en-US"));
    assert(!strcmp(user,"en-US"));
    setenv("LC_ALL","en_GB.UTF-8",1);
    assert(unix_to_win_locale("C",system));
    assert(!strcmp(system,"en-GB"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-locale-test-') as directory:
    path=pathlib.Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-o',str(path/'test'),str(path/'test.c')],check=True)
    subprocess.run([str(path/'test'),locale[1]],check=True)
print('PASS: launcher yields en-US for system and user even when libc stays in C')

# Optional original CD fixture stays outside the repository.
if len(sys.argv)>2:
    original=pathlib.Path(sys.argv[2]).read_bytes()
    start=launcher.index('static int ends_with(')
    end=launcher.index('/* Prefix setup and appearance',start)
    helper=launcher[start:end]
    with tempfile.TemporaryDirectory(prefix='quake-locale-test-') as directory:
        path=pathlib.Path(directory)
        (path/'INSTALL').mkdir()
        script=path/'INSTALL/SETUP.INS';script.write_bytes(original)
        c='''#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
'''+helper+'''
int main(int argc,char **argv) {
 return quake2_international_installer(argv[1]) != atoi(argv[2]);
}
'''
        (path/'profile.c').write_text(c)
        subprocess.run(['cc','-o',str(path/'profile'),str(path/'profile.c')],check=True)
        def check(target, expected):
            subprocess.run([str(path/'profile'),str(target),str(expected)],check=True)
        check(path/'SETUP.EXE',1)
        check(path/'INSTALL/SETUP.EXE',1)
        check(path/'INSTALL/OTHER.EXE',0)
        check(path/'OTHER/SETUP.EXE',0)
        script.write_bytes(bytes([original[0]^1])+original[1:])
        check(path/'SETUP.EXE',0)
        script.write_bytes(original+b'extra')
        check(path/'SETUP.EXE',0)
        script.write_bytes(original[:-1])
        check(path/'SETUP.EXE',0)
    print('PASS: only matching Quake II script selects the British locale; other installers excluded')
