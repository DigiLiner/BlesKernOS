"""Install the native-VM-compatible kernel into an offline disposable image.

Preserve stage 1 and all filesystem/user data. Native LBS mappings need the
kernel's dedicated RO return-gate page and personality-independent COW handler.
"""
from pathlib import Path
import re,struct
ROOT=Path(__file__).resolve().parents[1]
BOOT_BYTES=4096*512
KERNEL_BYTES=1280*1024

def expected_boot(original, directory=ROOT/'build'):
    result=bytearray(original)
    if len(result)!=BOOT_BYTES:raise ValueError('short boot area')
    directory=Path(directory)
    stage=(directory/'stage2.bin').read_bytes();kernel=(directory/'kernel.bin').read_bytes()
    if len(stage)!=4096 or not kernel or len(kernel)>KERNEL_BYTES:raise ValueError('invalid kernel/stage2 size')
    checksum=struct.pack('<I',sum(kernel)&0xffffffff)
    if not re.search(b'\x66\x81\x3e..'+re.escape(checksum),stage,re.S):
        raise ValueError('stage2 checksum does not match kernel; run make build/stage2.bin')
    result[512:512+len(stage)]=stage
    result[9*512:9*512+KERNEL_BYTES]=kernel.ljust(KERNEL_BYTES,b'\0')
    return bytes(result)

def install_kernel(image, directory=ROOT/'build'):
    with Path(image).open('r+b') as stream:
        original=stream.read(BOOT_BYTES);updated=expected_boot(original,directory)
        stream.seek(0);stream.write(updated)
