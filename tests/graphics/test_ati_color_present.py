#!/usr/bin/env python3
"""Host regression checks of Mesa resolve contracts and ATI Z16 clears."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
def extract(file, signature):
    s = (ROOT / file).read_text()
    a = s.index(signature)
    return s[a:s.index('\n}', a) + 2]
mesa = 'libs/Mesa-3.5/src/BlesKernOS/bkmesa_gpu.c'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int GLboolean;
typedef struct { int unused; } GLcontext;
#define GL_TRUE 1
#define GL_FALSE 0
typedef struct { int strict_failed, color_dirty, depth_dirty; } bkmesa_gpu_context;
static bkmesa_gpu_context gpu;
static int color_reads, depth_reads, fail_color;
static bkmesa_gpu_context *lookup(GLcontext *ctx) { return ctx ? &gpu : NULL; }
static GLboolean resolve_color(bkmesa_gpu_context *g) {
    if (fail_color) return GL_FALSE;
    if (g->color_dirty) { color_reads++; g->color_dirty = 0; }
    return GL_TRUE;
}
static GLboolean resolve_depth(bkmesa_gpu_context *g) {
    if (g->depth_dirty) { depth_reads++; g->depth_dirty = 0; }
    return GL_TRUE;
}
'''
code += extract(mesa, 'GLboolean bkmesa_gpu_resolve_color(GLcontext *ctx)')
code += extract(mesa, 'GLboolean bkmesa_gpu_resolve(GLcontext *ctx)')
code += r'''
#define MESA_BLESKERNOS 1
#define GLAPIENTRY
#define GLint int
#define GLushort unsigned short
#define GLuint unsigned int
typedef struct { int Width, Height; void *DepthBuffer; } framebuffer;
typedef struct { int depthBits; } visual;
typedef struct { GLcontext gl_ctx; framebuffer *gl_buffer; visual *gl_visual; } osmesa;
typedef osmesa *OSMesaContext;
'''
code += extract('libs/Mesa-3.5/src/OSmesa/osmesa.c',
                'GLboolean GLAPIENTRY\nOSMesaGetDepthBuffer(')
code += r'''
#define PERF_SCOPE_ATI_CLEAR 0
static uint64_t perfmon_scope_begin(void) { return 0; }
static void perfmon_scope_end(int s, uint64_t t) { (void)s; (void)t; }
static void perfmon_scope_note(int s, uint32_t u, int p, int t) {
    (void)s; (void)u; (void)p; (void)t;
}
typedef struct { uint32_t depth_page_count, depth_offset, depth_pitch, width, height; } r1283d_surface_t;
static union { uint32_t align; unsigned char bytes[2048]; } memory;
static struct { unsigned char *vram; } g_r1283d;
'''
code += extract('kernel/drivers/graphics/ati_rage128_3d_dvr.c', 'static void r1283d_clear_depth(')
code += r'''
int main(void) {
    GLcontext ctx;
    gpu.color_dirty = gpu.depth_dirty = 1;
    assert(bkmesa_gpu_resolve_color(&ctx));
    assert(color_reads == 1 && depth_reads == 0 && gpu.depth_dirty);
    assert(bkmesa_gpu_resolve_color(&ctx));
    assert(color_reads == 1 && gpu.depth_dirty);
    assert(bkmesa_gpu_resolve(&ctx));
    assert(depth_reads == 1 && !gpu.depth_dirty);
    gpu.color_dirty = gpu.depth_dirty = 1; fail_color = 1;
    assert(!bkmesa_gpu_resolve_color(&ctx) && gpu.depth_dirty);
    fail_color = 0; gpu.strict_failed = 1;
    assert(!bkmesa_gpu_resolve_color(&ctx));
    assert(!bkmesa_gpu_resolve_color(NULL));
    gpu.strict_failed = 0; gpu.color_dirty = 0; gpu.depth_dirty = 1;
    framebuffer fb = {8, 8, &ctx}; visual vis = {24}; osmesa os = {{0}, &fb, &vis};
    int width, height, bytes; void *buffer;
    assert(OSMesaGetDepthBuffer(&os, &width, &height, &bytes, &buffer));
    assert(width == 8 && height == 8 && bytes == 4 && buffer == &ctx);
    assert(depth_reads == 2 && !gpu.depth_dirty);
    gpu.strict_failed = 1;
    assert(!OSMesaGetDepthBuffer(&os, &width, &height, &bytes, &buffer));
    assert(!width && !height && !bytes && !buffer);
    g_r1283d.vram = memory.bytes;
    const float depths[] = {-1, 0, .5f, 1, 2};
    for (unsigned w = 1; w <= 127; w++) for (unsigned d = 0; d < 5; d++) {
        r1283d_surface_t s = {1, 32, (w * 2 + 31) & ~31U, w, 3};
        memset(memory.bytes, 0xA5, sizeof(memory.bytes));
        r1283d_clear_depth(&s, depths[d]);
        uint16_t expected = d < 2 ? 0 : d == 2 ? 32768 : 65535;
        for (unsigned i = 0; i < sizeof(memory.bytes); i++) {
            bool pixel = i >= 32 && i < 32 + s.height * s.depth_pitch &&
                         (i - 32) % s.depth_pitch < w * 2;
            unsigned char value = (i & 1) ? expected >> 8 : expected & 255;
            assert(memory.bytes[i] == (pixel ? value : 0xA5));
        }
    }
    puts("PASS: display resolve retains depth, explicit resolve synchronizes it; 635 Z16 clears preserve row padding and guards");
}
'''
with tempfile.TemporaryDirectory(prefix='ati-color-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
