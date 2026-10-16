#!/usr/bin/env python3
"""Exercise the real buffered BKL stream functions with a mocked VFS.

This compiles the function bodies extracted from kernel/setup/bkl_setup.c, so
changes to the implementation are exercised rather than a Python re-creation.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'kernel/setup/bkl_setup.c').read_text()


def section(begin: str, end: str) -> str:
    a = SRC.index(begin)
    b = SRC.index(end, a)
    return SRC[a:b]

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#define BKL_READ_BUFFER (32U * 1024U)
#define VFS_O_RDONLY 1
#define kmemcpy memcpy
#define kprintf(...) ((void)0)
typedef struct {
    uint32_t part_count;
    uint32_t part_index;
    int fd;
    uint32_t part_offset;
    const char *single_path;
    uint32_t total_read;
    uint32_t read_position;
    uint32_t read_available;
    uint32_t physical_reads;
} bkl_stream_t;
static uint8_t g_bkl_read_ahead[BKL_READ_BUFFER];
static uint8_t part0[200000], part1[200000];
static uint32_t sizes[2], positions[2];
static uint32_t open_count, close_count, read_count;
static int vfs_open(const char *path, uint32_t flags) {
    (void)flags;
    open_count++;
    if (strstr(path, "PART001")) return 4;
    return 3;
}
static bool vfs_close(int fd) { (void)fd; close_count++; return true; }
static int vfs_read(int fd, void *buffer, uint32_t bytes) {
    unsigned i = (unsigned)(fd - 3);
    if (i >= 2) return -1;
    read_count++;
    uint32_t left = sizes[i] - positions[i];
    if (bytes > left) bytes = left;
    if (bytes > 7000U) bytes = 7000U; /* exercise short VFS reads */
    memcpy(buffer, (i ? part1 : part0) + positions[i], bytes);
    positions[i] += bytes;
    return (int)bytes;
}
static void reset(uint32_t a, uint32_t b) {
    sizes[0] = a; sizes[1] = b;
    positions[0] = positions[1] = open_count = close_count = read_count = 0;
    for (uint32_t i = 0; i < a; i++) part0[i] = (uint8_t)(i * 13U + 7U);
    for (uint32_t i = 0; i < b; i++) part1[i] = (uint8_t)(i * 19U + 3U);
}
'''
code += section('static bool stream_open_part(', 'static bool stream_u8(')
code += r'''
int main(void) {
    bkl_stream_t s = {0};
    reset(200000, 0);
    s.fd = -1; s.part_count = 1; s.single_path = "/SETUP/CORE.BKL";
    assert(stream_open_part(&s, 0));
    for (uint32_t i = 0; i < 200000; i++) {
        uint8_t b = 0;
        assert(stream_read(&s, &b, 1));
        assert(b == part0[i]);
    }
    assert(s.total_read == 200000);
    assert(s.part_offset == 200000);
    assert(s.physical_reads < 40); /* old implementation needed 200,000 reads */
    assert(!stream_read(&s, part0, 1));
    assert(vfs_close(s.fd));
    printf("single part: 200000 byte-sized reads -> %u VFS reads\n", read_count);

    reset(10333, 20555);
    memset(&s, 0, sizeof(s));
    s.fd = -1; s.part_count = 2; s.single_path = NULL;
    assert(stream_open_part(&s, 0));
    uint8_t data[30888];
    assert(stream_read(&s, data, sizeof(data)));
    for (uint32_t i = 0; i < 10333; i++) assert(data[i] == part0[i]);
    for (uint32_t i = 0; i < 20555; i++) assert(data[i + 10333] == part1[i]);
    assert(s.part_index == 1);
    assert(s.part_offset == 20555);
    assert(s.total_read == sizeof(data));
    assert(open_count == 2 && close_count == 1);
    assert(!stream_read(&s, data, 1));
    printf("multi part: correct across boundary, %u VFS reads\n", read_count);

    reset(16, 0);
    memset(&s, 0, sizeof(s)); s.fd = -1; s.part_count = 1;
    s.single_path = "/SETUP/CORE.BKL";
    assert(stream_open_part(&s, 0));
    assert(!stream_read(&s, data, 17));
    printf("truncated input: rejected\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as d:
    src = Path(d) / 'test.c'
    exe = Path(d) / 'test'
    src.write_text(code)
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('BKL stream buffered tests PASS')
