#!/usr/bin/env python3
"""Exercise the actual Wine console launcher's command parser on the host."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = '#define WINE_TOOL_TEST\n#include "' + str(root / 'ports/wine/tools/wine-tool.c') + '"\n'
source += r'''
#include <assert.h>
#include <string.h>
int main(void) {
    char *args[8];
    char basic[] = "/SYSTEM/DEVEL/TCC/I386-TCC.EXE -c file.c -o file.o";
    assert(split_command(basic, args, 8) == 5);
    assert(!strcmp(args[0], "/SYSTEM/DEVEL/TCC/I386-TCC.EXE"));
    assert(!strcmp(args[4], "file.o"));
    char quoted[] = "tool \"a b.c\" \"\" C:\\TEMP\\file.c \"say \\\"hello\\\"\"";
    assert(split_command(quoted, args, 8) == 5);
    assert(!strcmp(args[1], "a b.c"));
    assert(!strcmp(args[2], ""));
    assert(!strcmp(args[3], "C:\\TEMP\\file.c"));
    assert(!strcmp(args[4], "say \"hello\""));
    char trailing[] = " tool  last \t ";
    assert(split_command(trailing, args, 8) == 2);
    assert(!strcmp(args[1], "last"));
    char invalid[] = "tool \"unfinished";
    assert(split_command(invalid, args, 8) == -1);
    char overflow[] = "one two three";
    assert(split_command(overflow, args, 3) == -1);
    char empty[] = " \t";
    assert(split_command(empty, args, 8) == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bles-wine-tool-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(source)
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS Wine tool parser: argv, quoting, empty args, Windows paths, bounds and malformed input')
