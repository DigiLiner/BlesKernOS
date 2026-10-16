#!/usr/bin/env python3
"""Ship rebuildable corresponding source for E100.DVR and copied Linux template."""
import io, tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MAKEFILE='''CC=gcc
CFLAGS=-march=pentium3 -mtune=pentium3 -m32 -mfpmath=387 -mno-sse -mno-sse2 -ffreestanding -fno-builtin -nostdlib -nostdinc -Os -Wall -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections
all: E100.DVR
E100.DVR: kernel/drivers/network/e100.c kernel/drivers/network/legacy_net.h kernel/drivers/network/legacy_net_runtime.inc kernel/drivers/legacy_io.h
	$(CC) $(CFLAGS) -c $< -o $@
'''
def build_family_source_archive(destination):
    destination=Path(destination);destination.parent.mkdir(parents=True,exist_ok=True)
    files=['kernel/drivers/network/e100.c','kernel/drivers/network/legacy_net.h',
           'kernel/drivers/network/legacy_net_runtime.inc','kernel/drivers/legacy_io.h',
           'kernel/include','third_party/linux-eepro100','LICENSES/GPL-2.0.txt','LICENSES/README.md',
           'docs/platform/family-driver-sources.json']
    with tarfile.open(destination,'w:gz') as tar:
        for name in files:tar.add(ROOT/name,arcname=name)
        tar.add(ROOT/'LICENSE',arcname='LICENSE.MIT')
        data=MAKEFILE.encode();info=tarfile.TarInfo('Makefile');info.mode=0o644;info.size=len(data)
        tar.addfile(info,io.BytesIO(data))
    return destination
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('destination',type=Path)
    print(build_family_source_archive(p.parse_args().destination))
