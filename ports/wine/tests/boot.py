#!/usr/bin/env python3
"""Regresion de boot normal con kernel actual; copia desechable del disco."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time
import sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
from kernel_image import KERNEL_MAX_BYTES,copy_boot_image
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--memory',type=int,default=64)
p.add_argument('--seconds',type=int,default=90)
p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-usb-user.img')
p.add_argument('--ahci-image',type=Path,help='copia desechable de un segundo disco SATA AHCI')
p.add_argument('--boot-ahci',action='store_true',help='arranca la imagen principal por SATA AHCI')
args=p.parse_args();out=args.output.resolve()
if args.memory<64:p.error('el kernel requiere al menos 64 MiB de RAM')
if args.boot_ahci and args.ahci_image:p.error('usar --boot-ahci o --ahci-image, no ambos')
if out.exists() and any(out.iterdir()):p.error('usar directorio nuevo')
out.mkdir(parents=True,exist_ok=True)
subprocess.run(['make','-j2','build/kernel.bin'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
kernel=(ROOT/'build/kernel.bin').read_bytes()
if len(kernel)>KERNEL_MAX_BYTES:raise RuntimeError('kernel supera limite de stage2')
stage=out/'stage2.bin'
subprocess.run(['nasm','-f','bin',f'-DKERNEL_SECTORS={(len(kernel)+511)//512}',f'-DKERNEL_CHECKSUM={sum(kernel)&0xffffffff}',
    'boot/stage2.asm','-o',str(stage)],cwd=ROOT,check=True)
image=out/'test.img';copy_boot_image(args.image,image)
with image.open('r+b') as disk:
    offset=struct.unpack_from('<I',disk.read(512),454)[0]*512
    disk.seek(offset);bpb=disk.read(512)
    if struct.unpack_from('<H',bpb,11)[0]!=512 or struct.unpack_from('<H',bpb,14)[0]*512<9*512+len(kernel):raise RuntimeError('geometria inesperada')
    disk.seek(offset+512);disk.write(stage.read_bytes());disk.seek(offset+9*512);disk.write(kernel)
command=['qemu-system-i386','-accel','tcg','-cpu','pentium3','-m',str(args.memory),'-smp','1',
    '-display','none','-serial',f'file:{out / "serial.log"}',
    '-monitor','none','-nic','none','-no-reboot','-no-shutdown']
if args.boot_ahci:
    command += ['-drive',f'file={image},format=raw,if=none,id=boot0',
                '-device','ich9-ahci,id=boot-ahci',
                '-device','ide-hd,drive=boot0,bus=boot-ahci.0']
else:
    command += ['-drive',f'file={image},format=raw,if=ide']
if args.ahci_image:
    ahci_image=out/'ahci.img'
    shutil.copyfile(args.ahci_image.resolve(),ahci_image)
    command += ['-drive',f'file={ahci_image},format=raw,if=none,id=sata0',
                '-device','ich9-ahci,id=ahci0',
                '-device','ide-hd,drive=sata0,bus=ahci0.0']
serial='';start=time.monotonic()
with (out/'qemu.log').open('w') as log:
    proc=subprocess.Popen(command,stdout=log,stderr=log)
    try:
        while proc.poll() is None and time.monotonic()-start<args.seconds:
            time.sleep(1)
            serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
            if 'GUI_RUN entered' in serial:break
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
result={'passed':'GUI_RUN entered' in serial and
        (not (args.ahci_image or args.boot_ahci) or '[AHCI] ahci0:' in serial),
        'memory_MiB':args.memory,'command':command}
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
if not result['passed']:raise SystemExit(1)
