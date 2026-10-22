#!/usr/bin/env python3
"""Exercise the production software fallback without per-primitive kernel queries."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'libs/Mesa-3.5/src/BlesKernOS/bkmesa_gpu.c').read_text()
start = source.index('static void before_cpu_reason(')
end = source.index('\nstatic void before_cpu(', start)
code = r'''
#include <assert.h>
#include <stdio.h>
typedef int GLint;
#define GL_FALSE 0
#define GL_TRUE 1
#define BK_CAP_DEPTH_SURFACE_IO 4
#define BKMESA_FALLBACK_NONE 0
#define BKMESA_FALLBACK_DEPTH_SYNC 1
typedef struct { unsigned capabilities; int render_gpu, cpu_dirty, cpu_depth_dirty, software_locked; } bkmesa_gpu_context;
static int queries, color_ok=1, depth_ok=1, fallback;
unsigned gfx3d_capabilities(void) { ++queries; return 4; }
static int resolve_color(bkmesa_gpu_context *g) { (void)g; return color_ok; }
static int resolve_depth(bkmesa_gpu_context *g) { (void)g; return depth_ok; }
static void mark_fallback(bkmesa_gpu_context *g, int reason) { (void)g; fallback=reason; }
''' + source[start:end] + r'''
int main(void) {
    bkmesa_gpu_context gpu={0};
    for (int i=0; i<10000; ++i) before_cpu_reason(&gpu, 0);
    assert(!queries && gpu.software_locked && gpu.cpu_dirty && gpu.cpu_depth_dirty && !gpu.render_gpu);
    gpu.capabilities=BK_CAP_DEPTH_SURFACE_IO;
    before_cpu_reason(&gpu, 0);
    assert(!gpu.software_locked && !queries);
    depth_ok=0; before_cpu_reason(&gpu, 0);
    assert(fallback==BKMESA_FALLBACK_DEPTH_SYNC);
    before_cpu_reason(&gpu, 7); assert(fallback==7);
    before_cpu_reason(0, 0);
    puts("PASS: software triangles use cached capabilities; depth synchronization and fallback reasons preserved");
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory)/'test.c'; binary=Path(directory)/'test'
    c.write_text(code)
    subprocess.run(['gcc','-Wall','-Wextra','-Werror','-O2',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
