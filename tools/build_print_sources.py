#!/usr/bin/env python3
"""Package corresponding GPL PrintSpool sources and a standalone i386 Makefile."""
from pathlib import Path
import io,tarfile
ROOT=Path(__file__).resolve().parents[1]
MAKEFILE='''CC=gcc
LD=ld
CFLAGS=-m32 -Os -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -ffunction-sections -fdata-sections -Isdk/include -Dmemchr=bk_print_memchr -Dabort=bk_print_jbig_abort
all: PRINTSPL.BEX
printspool.o: system/services/printspool.c
	$(CC) $(CFLAGS) -c $< -o $@
jbig.o: third_party/jbigkit/jbig.c
	$(CC) $(CFLAGS) -c $< -o $@
jbig_ar.o: third_party/jbigkit/jbig_ar.c
	$(CC) $(CFLAGS) -c $< -o $@
PRINTSPL.BEX: printspool.o jbig.o jbig_ar.o
	$(LD) -m elf_i386 -r --gc-sections -u bleskernos_program_main $^ -o $@
'''
def build_print_source_archive(destination):
 destination=Path(destination);destination.parent.mkdir(parents=True,exist_ok=True)
 with tarfile.open(destination,'w:gz') as archive:
  for relative in ['system/services/printspool.c','system/services/print_zjs.inc','system/services/print_families.inc',
                   'third_party/jbigkit','sdk/include','libs/src/font8x8_basic.h']:
   archive.add(ROOT/relative,arcname=relative)
  archive.add(ROOT/'LICENSE',arcname='LICENSE.MIT')
  payload=MAKEFILE.encode();info=tarfile.TarInfo('Makefile');info.size=len(payload);info.mode=0o644
  archive.addfile(info,io.BytesIO(payload))
 return destination
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('destination',type=Path)
 print(build_print_source_archive(parser.parse_args().destination))
