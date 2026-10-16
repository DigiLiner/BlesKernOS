#!/usr/bin/env python3
"""Smoke tests for CD Setup target classification, with fake block sectors.
No disk is written: compile the actual classifier taken from installer.c
against a minimal in-memory block mock. Requires a host C compiler.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'kernel/setup/installer.c').read_text()
BKL = (ROOT / 'kernel/setup/bkl_setup.c').read_text()
MAKE = (ROOT / 'Makefile').read_text()

start = SOURCE.index('static uint32_t installer_le32(')
stop = SOURCE.index('/* Expose MBR primary partitions', start) if '/* Expose MBR primary partitions' in SOURCE[start:] else SOURCE.index('/* Partition views are created', start)
functions = SOURCE[start:stop]
# Do not include implementation of unrelated static helpers between the two
# snippets; only the LE32 reader and MBR classifier are compiled.
reader = SOURCE[SOURCE.index('static uint32_t installer_le32('):SOURCE.index('static bool installer_partition_type_supported(')]
classifier = SOURCE[SOURCE.index('typedef enum {\n    INSTALL_DISK_EMPTY,'):SOURCE.index('/* Partition views are created')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define BLOCK_SECTOR_SIZE 512
#define kmemcpy memcpy
typedef struct { uint32_t sector_count; } block_device_t;
static uint8_t disk[2][512];
static bool fail_second;
static bool block_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    (void)dev;
    if (count != 1 || lba > 1 || (lba == 1 && fail_second)) return false;
    memcpy(buffer, disk[lba], 512);
    return true;
}
'''+reader+classifier+r'''
static void le32(uint8_t *p, uint32_t value) {
  p[0]=value; p[1]=value>>8; p[2]=value>>16; p[3]=value>>24;
}
static void partition(int idx,int type,uint32_t first,uint32_t size,int active) {
    uint8_t *p=disk[0]+446+16*idx;
    p[0]=active; p[4]=type; le32(p+8,first); le32(p+12,size);
}
#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "failure line %d: %s\n", __LINE__, #x);return 1;} }while(0)
int main(void) {
    block_device_t dev={300000};
    memset(disk,0,sizeof(disk));
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_EMPTY);
    disk[0][510]=0x55; disk[0][511]=0xaa;
    partition(0,0x0b,2048,100000,0x80);
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_MBR);
    partition(1,0x83,50000,80000,0);
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE); /* overlap */
    partition(1,0,0,0,0);
    partition(0,0xEE,1,299999,0);
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE);
    memset(disk,0,sizeof(disk));
    memcpy(disk[1],"EFI PART",8);
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE); /* GPT w/o protective MBR */
    memset(disk,0,sizeof(disk));
    disk[0][510]=0x55; disk[0][511]=0xaa;
    partition(0,0x0b,2048,100000,0x01);
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE); /* invalid boot flag */
    partition(0,0x0b,2048,100000,0x80);
    disk[0][510]=0;
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE);
    disk[0][510]=0x55;
    fail_second=true;
    EXPECT(installer_disk_table(&dev,0)==INSTALL_DISK_UNSAFE); /* failed read */
    puts("PASS: blank, MBR, GPT, overlap, signature, invalid flag, IO failure");
    return 0;
}
'''

assert 'installer_clear_owned_tree' not in SOURCE
assert 'DOCUMENTS"' not in SOURCE[SOURCE.index('static bool installer_copy_tree'):]
assert 'installer_space_preflight(target, mode)' in SOURCE
assert 'installer_confirm_mbr_change()' in SOURCE
assert 'installer_manifest_unpacked(&raw)' in SOURCE
assert 'kmalloc(original_size)' not in BKL
assert 'vfs_rename(writer->temp_path, path)' in BKL
assert 'MBR.BIN' in MAKE and 'INSTALL_TARGET_MB ?= 1536' in MAKE

with tempfile.TemporaryDirectory() as temporary:
    path = Path(temporary) / 'test.c'
    binary = Path(temporary) / 'test'
    path.write_text(code)
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: instalador safety smoke checks')
