#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
from kernel_image import KERNEL_MAX_BYTES,KERNEL_MAX_SECTORS

root = Path.cwd()
kernel_bin = root / "build/kernel.bin"
kernel_elf = root / "build/kernel.elf"
stage2_bin = root / "build/stage2.bin"
stage2_src = root / "boot/stage2.asm"
memory_h = root / "kernel/include/memory.h"
memory_c = root / "kernel/mm/memory.c"
errors = []

if not kernel_bin.exists(): errors.append("falta build/kernel.bin")
if not kernel_elf.exists(): errors.append("falta build/kernel.elf")
if errors:
    for error in errors: print("ERROR:", error)
    raise SystemExit(1)

size = kernel_bin.stat().st_size
sectors = (size + 511) // 512
print(f"kernel.bin       : {size} bytes ({sectors} sectores)")
print("destino kernel   : 0x00100000")
print(f"fin kernel plano : 0x{0x00100000 + size:08X}")
print(f"limite PROGBITS  : 0x{0x100000+KERNEL_MAX_BYTES:08X}")
if size > KERNEL_MAX_BYTES:
    errors.append(f"kernel.bin supera reserva por {size-KERNEL_MAX_BYTES} bytes")

result = subprocess.run(
    ["readelf", "-SW", str(kernel_elf)],
    check=True, capture_output=True, text=True,
)
sections = {}
pattern = re.compile(
    r"\[\s*\d+\]\s+(\.\S+)\s+\S+\s+"
    r"([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+([0-9A-Fa-f]+)"
)
for line in result.stdout.splitlines():
    match = pattern.search(line)
    if match:
        sections[match.group(1)] = (int(match.group(2), 16), int(match.group(3), 16))

for required in (".boot", ".text", ".data", ".bss"):
    if required not in sections: errors.append(f"no encontré {required} en kernel.elf")
if ".boot" in sections:
    address, length = sections[".boot"]
    if address != 0x00100000 or address + length > 0x00110000:
        errors.append(".boot debe permanecer en 0x00100000..0x0010ffff")
if ".text" in sections:
    address, _ = sections[".text"]
    print(f".text            : 0x{address:08X}")
    if address != 0x00110000: errors.append(".text residente no comienza en 0x00110000")
if ".data" in sections:
    address, length = sections[".data"]
    print(f".data            : 0x{address:08X} .. 0x{address + length:08X}")
    if address + length > 0x100000+KERNEL_MAX_BYTES: errors.append(".data supera reserva")
if ".bss" in sections:
    address, length = sections[".bss"]
    print(f".bss             : 0x{address:08X} .. 0x{address + length:08X}")
    if address != 0x02000000: errors.append("BSS no comienza tras la ventana de imagenes Wine")
    if address + length > 0x02400000: errors.append("BSS invade el heap de 36 MiB")

stage2_text = stage2_src.read_text()
makefile=(root/'Makefile').read_text()
if not re.search(r'KERNEL_STAGE2_SECTORS\s*:=\s*'+str(KERNEL_MAX_SECTORS)+r'\b',makefile):
    errors.append('Makefile y reserva kernel no coinciden')
if not re.search(r'FAT_RESERVED_SECTORS\s*:=\s*'+str(9+KERNEL_MAX_SECTORS)+r'\b',makefile):
    errors.append('reserva FAT base no coincide (builder agrega 3 sectores metadata)')
for name,constant in [('installer.c','INSTALL_KERNEL_RESERVED_SECTORS'),('floppy_installer.c','FI_KERNEL_RESERVED_SECTORS')]:
    if not re.search(constant+r'\s+'+str(KERNEL_MAX_SECTORS)+r'U?\b',(root/'kernel'/'setup'/name).read_text()):
        errors.append(name+': reserva kernel inconsistente')
if not re.search(r"KERNEL_LOAD_ADDR\s+equ\s+0x00100000", stage2_text):
    errors.append("Stage 2 no apunta a 0x00100000")
if not re.search(r"KERNEL_BOUNCE_ADDR\s+equ\s+0x00010000", stage2_text):
    errors.append("Stage 2 no tiene bounce buffer en 0x10000")
memory_layout = memory_h.read_text() + "\n" + memory_c.read_text()
if 'MM_MIN_SUPPORTED_RAM (63U * 1024U * 1024U)' not in memory_layout:
    errors.append("falta umbral E820 para 64 MiB instalados")
if not re.search(r'mov esp, 0x00400000', stage2_text):
    errors.append("Stage 2 no reserva la pila bootstrap bajo 4 MiB")
if 'PE_FIXED_VIEW_START 0x00400000U' not in memory_layout:
    errors.append("revisar vista PE historica")
if stage2_bin.exists():
    stage2_size = stage2_bin.stat().st_size
    print(f"stage2.bin       : {stage2_size} bytes")
    if stage2_size != 4096: errors.append(f"Stage 2 mide {stage2_size}, debería medir 4096 bytes")
if sectors > KERNEL_MAX_SECTORS: errors.append("kernel.bin supera reserva de sectores")

if errors:
    print()
    for error in errors: print("ERROR:", error)
    raise SystemExit(1)
print()
print("OK: layout HighMem consistente")
