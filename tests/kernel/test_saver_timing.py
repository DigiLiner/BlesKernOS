#!/usr/bin/env python3
"""Check saver pacing with 100/300 Hz clocks, stalls and clock wraparound."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CODE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "system/screensavers/ss_timing.h"
static void check_clock(uint32_t hz, uint32_t origin) {
    uint32_t last = origin, frames = 0;
    for (uint32_t tick = 0; tick <= hz * 10; tick++) {
        uint32_t now = origin + tick * 1000 / hz;
        if (ss_frame_due(now, &last)) frames++;
        assert(!ss_frame_due(now, &last));
    }
    assert(frames == 10000 / SS_FRAME_MS);
}
int main(void) {
    check_clock(100, 0);
    check_clock(300, 0);
    check_clock(100, UINT32_MAX - 500);
    check_clock(300, UINT32_MAX - 500);
    uint32_t last = 0;
    assert(!ss_frame_due(32, &last));
    assert(ss_frame_due(33, &last) && last == 33);
    assert(ss_frame_due(5000, &last));
    assert(!ss_frame_due(5000, &last));
    assert(ss_frame_due(5016, &last));
    puts("PASS: 30 FPS pacing at 100/300 Hz, no drift, wraparound, no catch-up burst");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-saver-timing-') as directory:
    source = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    source.write_text(CODE)
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-fsanitize=undefined', '-I' + str(ROOT), str(source),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
