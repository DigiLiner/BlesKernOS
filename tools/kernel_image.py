"""Reserva del kernel y copia migrada de discos de prueba FAT32/MBR.
Nunca modifica el disco fuente. La numeracion de clusters no cambia.
"""
from pathlib import Path
import shutil
import struct

KERNEL_MAX_BYTES=1280*1024
KERNEL_MAX_SECTORS=KERNEL_MAX_BYTES//512
FAT_RESERVED_SECTORS=9+KERNEL_MAX_SECTORS+3

def copy_boot_image(source,target):
    source,target=Path(source).resolve(),Path(target).resolve()
    if source==target or target.exists():raise ValueError('la copia requiere destino nuevo')
    with source.open('rb') as src:
        mbr=bytearray(src.read(512))
        if mbr[510:512]!=b'\x55\xaa':raise ValueError('MBR invalido')
        start,total=struct.unpack_from('<II',mbr,454)
        if not start or any(mbr[462:510]):raise ValueError('solo una particion primaria')
        if (start+total)*512!=source.stat().st_size:raise ValueError('particion no termina en EOF')
        offset=start*512;src.seek(offset);boot=bytearray(src.read(512))
        old=struct.unpack_from('<H',boot,14)[0]
        if old>=total:raise ValueError('reserva fuera de la particion')
        if struct.unpack_from('<H',boot,11)[0]!=512 or struct.unpack_from('<I',boot,36)[0]==0:
            raise ValueError('se requiere FAT32 con sectores de 512 bytes')
        if old>=FAT_RESERVED_SECTORS:
            shutil.copyfile(source,target);return
        info,backup=struct.unpack_from('<HH',boot,48)
        if info<9 or backup<9 or info>=old or backup+1>=old:raise ValueError('metadata inesperada')
        src.seek(offset+info*512);fsinfo=src.read(512)
        extra=FAT_RESERVED_SECTORS-old
        struct.pack_into('<I',mbr,458,total+extra)
        struct.pack_into('<H',boot,14,FAT_RESERVED_SECTORS)
        struct.pack_into('<I',boot,32,total+extra)
        struct.pack_into('<HH',boot,48,FAT_RESERVED_SECTORS-3,FAT_RESERVED_SECTORS-2)
        with target.open('xb') as dst:
            src.seek(0)
            remaining=offset+old*512
            while remaining:
                chunk=src.read(min(1024*1024,remaining));dst.write(chunk);remaining-=len(chunk)
            dst.write(bytes(extra*512))
            shutil.copyfileobj(src,dst,1024*1024)
            dst.seek(0);dst.write(mbr)
            dst.seek(offset);dst.write(boot)
            # Borrar metadata vieja; esta zona ahora pertenece a la reserva kernel.
            for sector in {info,backup,backup+1}:
                dst.seek(offset+sector*512);dst.write(bytes(512))
            for sector,data in [(FAT_RESERVED_SECTORS-3,fsinfo),(FAT_RESERVED_SECTORS-2,boot),(FAT_RESERVED_SECTORS-1,fsinfo)]:
                dst.seek(offset+sector*512);dst.write(data)
