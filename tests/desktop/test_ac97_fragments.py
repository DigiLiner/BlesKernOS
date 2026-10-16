#!/usr/bin/env python3
"""Run the real AC97 descriptor filler with short/full DMA fragments."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/audio/ac97.c').read_text()
def function(signature):
    start = source.index(signature + ' {')
    end = source.index('\n}', start) + 2
    return source[start:end]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define AC97_BDL_COUNT 32U
#define AC97_BUFFER_FRAMES 2048U
#define AC97_CONTROLLER_SIS7012 1
#define kmemset memset
static struct { int type; } controller, *g_ac97_controller = &controller;
static struct { uint16_t samples; } g_ac97_bdl[32];
static int16_t buffers[32][4096], *g_ac97_buffers[32];
static uint32_t pending;
static uint32_t ac97_render(int16_t *out, uint32_t max) {
    uint32_t n = pending < max ? pending : max;
    pending -= n;
    for (uint32_t i = 0; i < n * 2; ++i) out[i] = 123;
    return n;
}
''' + function('static uint16_t ac97_descriptor_length(uint32_t frames)') + '\n' + function('static bool ac97_fill_descriptor(uint32_t sequence)') + r'''
int main(void) {
    for (unsigned i = 0; i < 32; ++i) g_ac97_buffers[i] = buffers[i];
    /* A 100-ms fragment must describe 4800 frames, not 6144. */
    pending = 4800;
    assert(ac97_fill_descriptor(0)); assert(g_ac97_bdl[0].samples == 4096);
    assert(ac97_fill_descriptor(1)); assert(g_ac97_bdl[1].samples == 4096);
    assert(ac97_fill_descriptor(2)); assert(g_ac97_bdl[2].samples == 1408);
    assert(buffers[2][1406] == 123 && buffers[2][1408] == 0);
    assert(!ac97_fill_descriptor(3));
    /* Reusing a short slot must restore the full length. */
    pending = 2048; assert(ac97_fill_descriptor(34));
    assert(g_ac97_bdl[2].samples == 4096);
    controller.type = AC97_CONTROLLER_SIS7012;
    pending = 704; assert(ac97_fill_descriptor(2));
    assert(g_ac97_bdl[2].samples == 2816);
    puts("PASS AC97 exact short-fragment duration, ring reuse, SiS byte units");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-ac97-') as directory:
    c = Path(directory) / 'test.c'; c.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
