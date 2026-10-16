#!/usr/bin/env python3
"""Create a separate bootable legacy-driver image from an existing BFS4 build."""
import argparse,subprocess
from pathlib import Path
from bfs4_update import Editor
from build_print_sources import build_print_source_archive
from build_family_sources import build_family_source_archive
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
p.add_argument('--output',type=Path,default=ROOT/'build/bleskernos-legacy-drivers.img')
a=p.parse_args()
if a.base.resolve()==a.output.resolve():p.error('output must differ from the base image')
if a.output.exists():p.error('output exists; choose a new output path')
subprocess.run(['make','legacy-drivers','build/system/drivers/USBCLASS.DVR','build/system/services/printspool.o','build/programs/texteditor.o','build/stage2.bin'],cwd=ROOT,check=True)
a.output.parent.mkdir(parents=True,exist_ok=True)
subprocess.run(['cp','--reflink=auto',str(a.base),str(a.output)],check=True)
with a.output.open('r+b') as disk:
 disk.seek(512);disk.write((ROOT/'build/stage2.bin').read_bytes())
 kernel=(ROOT/'build/kernel.bin').read_bytes()
 if len(kernel)>1280*1024:raise ValueError('kernel exceeds reserved area')
 disk.seek(9*512);disk.write(kernel.ljust(1280*1024,b'\0'))
e=Editor(a.output)
e.mkdir('/SYSTEM/FIRMWARE')
for driver in sorted((ROOT/'build/system/drivers').glob('*.DVR')):
 e.update('/SYSTEM/DRIVERS/'+driver.name,driver.read_bytes())
for profile in sorted((ROOT/'system/printers').glob('*.BPD')):
 e.update('/SYSTEM/PRINTERS/'+profile.name,profile.read_bytes())
for destination,source in [('/SYSTEM/SERVICES/PRINTSPL.BEX','build/system/services/printspool.o'),
 ('/SYSTEM/PROGRAMS/TEXTEDITOR.BEX','build/programs/texteditor.o'),
 ('/DOCS/LEGACYDRIVERS.TXT','docs/platform/legacy-drivers.md'),
 ('/DOCS/DRIVERTESTS.TXT','docs/driver-expansion-2026-10-13.md'),
 ('/DOCS/DRIVERSPLIT.TXT','docs/driver-source-layout.md'),
 ('/DOCS/DRIVERSOURCES.JSON','docs/platform/legacy-driver-sources.json'),
 ('/DOCS/PRINTING.TXT','docs/platform/printing.md'),('/DOCS/USBCLASS.TXT','docs/platform/usbclass.md'),
 ('/DOCS/FAMILYDRIVERS.TXT','docs/platform/family-drivers.md'),
 ('/DOCS/FAMILYTESTS.TXT','docs/family-expansion-2026-10-13.md'),
 ('/DOCS/FAMILYSOURCES.JSON','docs/platform/family-driver-sources.json'),
 ('/DOCS/FRAMEBUFFERIDS.JSON','docs/platform/framebuffer-device-catalog.json'),
 ('/DOCS/NETGPL.TXT','LICENSES/GPL-2.0.txt'),
 ('/DOCS/PCIIDSLICENSE.TXT','third_party/pciids/COPYING'),
 ('/DOCS/PRINTGPL.TXT','third_party/jbigkit/COPYING'),('/SYSTEM/FIRMWARE/README.TXT','system/firmware/README.md')]:
 e.update(destination,(ROOT/source).read_bytes())
family_source=build_family_source_archive(ROOT/'build/FAMILYSRC.TGZ');e.update('/DOCS/FAMILYSRC.TGZ',family_source.read_bytes())
source=build_print_source_archive(ROOT/'build/PRINTSRC.TGZ');e.update('/DOCS/PRINTSRC.TGZ',source.read_bytes())
for filename in ['SIHP1000.DL','SIHP1005.DL','SIHP1018.DL','SIHP1020.DL']:
 source=ROOT/'system/firmware'/filename
 if source.is_file():e.update('/SYSTEM/FIRMWARE/'+filename,source.read_bytes())
e.finish();print('Created:',a.output)
