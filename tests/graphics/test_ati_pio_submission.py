#!/usr/bin/env python3
"""Host checks of the actual ATI PIO serializer. Does not emulate a Rage 128."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/drivers/graphics/ati_rage128_3d_dvr.c').read_text()
start = source.index('static bool r1283d_submit_words(')
end = source.index('\n}', start) + 2
function = source[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define R1283D_PIO_FIFO_DWORDS 192U
#define R128_PM4_FIFO_DATA_EVEN 0U
#define R128_PM4_FIFO_DATA_ODD 1U
#define R128_CCE_PACKET2 0x80000000U
#define PERF_SCOPE_ATI_SUBMIT 0U
static uint32_t reservation, writes, waits, fail_at, data[4096];
static uint32_t units;
static uint64_t perfmon_scope_begin(void) { return 1; }
static void perfmon_scope_end(unsigned s, uint64_t t) { (void)s; (void)t; }
static void perfmon_scope_note(unsigned s, uint32_t n, unsigned p, unsigned t) {
    (void)s; (void)p; (void)t; units += n;
}
static bool r1283d_wait_pio_slots(uint32_t slots) {
    assert(!reservation && slots && !(slots & 1U) && slots <= 192U);
    waits++;
    if (waits == fail_at) return false;
    reservation = slots;
    return true;
}
static void r1283d_write(uint32_t reg, uint32_t value) {
    assert(reservation && reg == (writes & 1U));
    reservation--;
    data[writes++] = value;
}
'''
harness += function + r'''
int main(void) {
    uint32_t input[3000];
    for (unsigned i = 0; i < 3000; i++) input[i] = 0x10000000U + i;
    for (unsigned n = 1; n <= 3000; n++) {
        reservation = writes = waits = fail_at = units = 0;
        assert(r1283d_submit_words(input, n));
        assert(writes == ((n + 1U) & ~1U) && !reservation);
        assert(waits == (n + 191U) / 192U && units == n);
        for (unsigned i = 0; i < n; i++) assert(data[i] == input[i]);
        if (n & 1U) assert(data[n] == R128_CCE_PACKET2);
    }
    reservation = writes = waits = units = 0; fail_at = 2;
    assert(!r1283d_submit_words(input, 500));
    assert(writes == 192 && waits == 2 && units == 192 && !reservation);
    assert(!r1283d_submit_words(NULL, 2));
    assert(!r1283d_submit_words(input, 0));
    puts("PASS: 3000 packet lengths, exact stream/padding, bounded reservations, failed wait stops writes");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ati-pio-test-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,address', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
