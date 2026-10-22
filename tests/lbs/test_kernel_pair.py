#!/usr/bin/env python3
"""Reject stale boot checksums without changing the source image."""
import sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from lbs_kernel import BOOT_BYTES,KERNEL_BYTES,expected_boot
with tempfile.TemporaryDirectory() as tmp:
 directory=Path(tmp)
 kernel=b'\x01\x02\x03'
 stage=bytearray(4096)
 stage[100:110]=b'\x66\x81\x3e\x00\x10\x06\x00\x00\x00\x90'
 (directory/'stage2.bin').write_bytes(stage)
 (directory/'kernel.bin').write_bytes(kernel)
 original=b'\xa5'*BOOT_BYTES
 result=expected_boot(original,directory)
 assert result[:512]==original[:512]
 assert result[512:4608]==stage
 assert result[4608:4608+KERNEL_BYTES]==kernel.ljust(KERNEL_BYTES,b'\0')
 assert result[4608+KERNEL_BYTES:]==original[4608+KERNEL_BYTES:]
 (directory/'kernel.bin').write_bytes(kernel+b'\x01')
 try:expected_boot(original,directory)
 except ValueError as error:assert 'checksum' in str(error)
 else:raise AssertionError('accepted stale stage2')
 assert original==b'\xa5'*BOOT_BYTES
print('PASS: current boot pair, preserved stage1/unused boot bytes, stale checksum rejected')
