#!/usr/bin/env python3
"""Reproduce ELF alignment/allocation arithmetic without loading any image.

This is a vulnerability diagnostic, not a passing safety regression. It runs
the production helper and allocation-size expression on a 32-bit host.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/exec/elf_loader.c').read_text()
helper = source[source.index('static uint32_t elf_align('):source.index('static bool g_loading_vision_service;')]
expression = re.search(r'kzalloc\((image_size \+ image_alignment - 1U \+\s*sizeof\(void \*\))\)', source).group(1)
code = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
''' + helper + r'''
int main(void) {
    assert(sizeof(void *)==4);
    assert(elf_align(17U,16U)==32U);
    uint32_t wrapped=elf_align(0xfffffff8U,16U);
    assert(wrapped==0U);
    /* This alignment passes the existing power-of-two test. The accumulated
     * section length passes its separate overflow test from an offset of 0. */
    uint32_t image_size=0x7fffffffU,image_alignment=0x80000000U;
    assert(!(image_alignment&(image_alignment-1U)));
    assert(elf_align(0U,image_alignment)==0U);
    uint32_t allocated_bytes=''' + expression + r''';
    assert(allocated_bytes==2U && allocated_bytes<image_size);
    printf("{\"diagnostic\":\"ELF arithmetic\",\"alignment_wrap_observed\":true,\"section_bytes\":%u,\"allocation_bytes\":%u,\"environment\":\"i386 host arithmetic, no malformed ELF loaded\"}\n",image_size,allocated_bytes);
}
'''
with tempfile.TemporaryDirectory(prefix='bk-elf-audit-') as directory:
    c = Path(directory) / 'audit.c'
    binary = Path(directory) / 'audit'
    c.write_text(code)
    subprocess.run(['gcc', '-m32', '-O2', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
