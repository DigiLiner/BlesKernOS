#!/usr/bin/env python3
"""Compile Wine's actual no-XInput2 clipping path and exercise its event feedback.

Usage: python3 ports/wine/tests/test_cursor_clip_loop.py /path/to/wine-source
Only the prefix before XI2-dependent X calls is needed for this regression.
"""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]) / 'dlls/winex11.drv/mouse.c'
text = source.read_text()
start = text.index('static BOOL grab_clipping_window( const RECT *clip )')
end = text.index('    /* enable XInput2 unless we are already clipping */', start)
prefix = text[start:end] + '    return TRUE;\n#endif\n}\n'
shim = r'''
#include <assert.h>
#include <stddef.h>
#define HAVE_X11_EXTENSIONS_XINPUT2_H
#define TRUE 1
#define FALSE 0
#define WARN(...) ((void)0)
typedef int BOOL;
typedef int Window;
typedef int HCURSOR;
typedef struct { int x,y; } POINT;
typedef struct { int left,top,right,bottom; } RECT;
struct x11drv_thread_data { int unused; };
static struct x11drv_thread_data thread;
static int keyboard_grabbed, xinput2_available, pending_unclip;
static struct x11drv_thread_data *x11drv_thread_data(void) { return &thread; }
static int NtUserGetDesktopWindow(void) { return 1; }
static int NtUserGetWindowThread(int w, void *p) { return 2; }
static int GetCurrentThreadId(void) { return 3; }
static int is_current_process_focused(void) { return 1; }
static int init_clip_window(void) { return 1; }
static void NtUserClipCursor(void *r) { pending_unclip++; }
'''
# win32u retries fullscreen clipping after an unclip notification. The driver
# must fail without generating that notification when XI2 is unavailable.
harness = r'''
int main(void) {
    RECT rect = {0,0,760,536};
    int iterations=0, retry=1, result=TRUE;
    while (retry && iterations++ < 64) {
        result=grab_clipping_window(&rect);
        retry=pending_unclip != 0;
        pending_unclip=0;
    }
    assert(iterations==1);
    assert(result==FALSE);
    assert(pending_unclip==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-clip-test-') as directory:
    root = pathlib.Path(directory)
    for label in ['fixed', 'original']:
        body=prefix
        # Reconstruct the original behavior specifically in the XI2 branch.
        if label == 'original':
            begin=prefix.index('    if (!xinput2_available)')
            finish=prefix.index('\n    }', begin)+6
            body=prefix[:begin]+'''    if (!xinput2_available)
    {
        NtUserClipCursor(NULL);
        return TRUE;
    }'''+prefix[finish:]
        cfile=root/(label+'.c'); binary=root/label
        cfile.write_text(shim+body+harness)
        subprocess.run(['cc','-Wno-unused-function','-o',str(binary),str(cfile)],check=True)
        result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        assert (result.returncode==0)==(label=='fixed'), (label,result.stderr)
print('PASS: no-XInput2 fullscreen clip settles; original feedback loop rejected')
