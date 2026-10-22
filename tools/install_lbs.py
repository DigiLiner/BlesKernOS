#!/usr/bin/env python3
"""Install the built LBS platform into an offline BFS4 copy, atomically.

Default output is a separate bootable image, preserving the original image and
its user configuration. Stage 1, Wine prefix and startup policy are preserved;
the current kernel/stage 2 are installed for native VM/COW support.
"""
import argparse,json,os,shutil,subprocess,tempfile
from pathlib import Path
from bfs4_update import Editor
from lbs_kernel import install_kernel
from lbs_payload import application_payloads, remove_old_libraries
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--source',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 p.add_argument('--output',type=Path,default=ROOT/'build/bleskernos-lbs-user.img')
 p.add_argument('--kernel-dir',type=Path,default=ROOT/'build')
 p.add_argument('--tests',action='store_true');p.add_argument('--dry-run',action='store_true');a=p.parse_args()
 libraries=json.loads((ROOT/'sdk/lbs/dependencies.json').read_text())
 files={f'/SYSTEM/{n}.LBS':ROOT/f'build/system/libs/native/{n}.LBS' for n in libraries}
 files.update({
  '/SYSTEM/SERVICES/VISION.BEX':ROOT/'build/system/services/vision.o',
  '/SYSTEM/DRIVERS/TLS.DVR':ROOT/'build/system/drivers/TLS.DVR',
  '/SYSTEM/PROGRAMS/LBSTRI.BEX':ROOT/'build/tests/userland/LBSTRI.BEX',
  '/SYSTEM/SDK/EXAMPLES/LBS_TRI.C':ROOT/'sdk/examples/lbs_triangle.c',
  '/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX':ROOT/'build/lbs/apps/programs/network_status.o',
  '/SYSTEM/SDK/LIB/LIBLBS.A':ROOT/'build/sdk/libblesk_lbs.a',
  '/SYSTEM/SDK/LIB/LIBPOSIX.A':ROOT/'build/sdk/libblesk_posix.a',
  '/SYSTEM/SDK/LIB/LIBBLESK.A':ROOT/'build/sdk/libblesk.a',
  '/SYSTEM/SDK/LIB/LBSHOST.O':ROOT/'build/sdk/lbs_host.o',
  '/SYSTEM/SDK/EXAMPLES/LBS_DEMO.C':ROOT/'sdk/examples/lbs_demo.c',
  '/SYSTEM/SDK/EXAMPLES/LBS_START.C':ROOT/'sdk/examples/lbs_startup_demo.c',
  '/SYSTEM/DOCS/LBS.TXT':ROOT/'docs/native/LBS.md',
  '/SYSTEM/DOCS/LBSSHARE.TXT':ROOT/'docs/native/LBS-SHARING.md',
  '/SYSTEM/DOCS/LBSMIG.TXT':ROOT/'docs/native/LBS-MIGRATION.md',
  '/SYSTEM/DOCS/LBSTOTAL.TXT':ROOT/'docs/native/LBS-TOTAL.md',
  '/SYSTEM/DOCS/LBSREPORT.TXT':ROOT/'docs/native/LBS-REPORT.md',
  '/SYSTEM/DOCS/LBSAUDIT.TXT':ROOT/'docs/native/LBS-AUDIT.md',
  '/SYSTEM/DOCS/LBSEXPORT.TXT':ROOT/'docs/native/LBS-EXPORTS.md',
 })
 files.update(application_payloads())
 files['/SYSTEM/SDK/LIB/LIBLBSPX.A']=ROOT/'build/sdk/libblesk_lbs_posix.a'
 files['/SYSTEM/SDK/LIB/LIBLBSNT.A']=ROOT/'build/sdk/libblesk_lbs_native.a'

 for header in (ROOT/'sdk/include').glob('*.h'):files['/SYSTEM/SDK/INCLUDE/'+header.name.upper()]=header
 if a.tests:
  files['/SYSTEM/TESTS/NATIVE/LBSTEST.BEX']=ROOT/'build/tests/userland/LBSTEST.BEX'
  files['/SYSTEM/TESTS/NATIVE/LBSSTART.BEX']=ROOT/'build/tests/userland/LBSSTART.BEX'
  files['/SYSTEM/TESTS/NATIVE/LBSAPPS.BEX']=ROOT/'build/tests/userland/LBSAPPS.BEX'
  files['/SYSTEM/TESTS/NATIVE/POSIX_TEST.BEX']=ROOT/'build/programs/posix_test.o'
  files['/SYSTEM/TESTS/NATIVE/SDL_DEMO.BEX']=ROOT/'build/programs/sdl_demo.o'
  for name in ['DLMOD.SO','DLDEP.SO']:files['/SYSTEM/TESTS/NATIVE/'+name]=ROOT/'build/tests/userland'/name
  for lib in (ROOT/'build/tests/lbs').glob('*.LBS'):files['/SYSTEM/TESTS/NATIVE/'+lib.name]=lib
  files['/SYSTEM/TESTS/NATIVE/FRAME.GIF']=ROOT/'build/tests/lbs/FRAME.GIF'
  files['/SYSTEM/CORRUPT.LBS']=ROOT/'build/tests/lbs/CORRUPT.BAD'
 for source in files.values():
  if not source.is_file():raise FileNotFoundError(f'{source}; run make lbs-apps lbs-audit first')
 if a.source.resolve()==a.output.resolve():p.error('source and output must differ; installation preserves the original image')
 for guest,host in sorted(files.items()):print(f'{guest}: {host.stat().st_size} bytes')
 if a.dry_run:return
 a.output.parent.mkdir(parents=True,exist_ok=True)
 # Refuse replacing an output image currently opened by QEMU.
 for proc in Path('/proc').iterdir():
  if not proc.name.isdecimal():continue
  try:
   if 'qemu-system' not in (proc/'comm').read_text():continue
   for fd in (proc/'fd').iterdir():
    try:
     if fd.resolve() in (a.output.resolve(),a.source.resolve()):raise RuntimeError('stop QEMU before copying or replacing its disk image')
    except OSError:pass
  except (OSError,PermissionError):pass
 fd,tmp=tempfile.mkstemp(prefix=a.output.name+'.',suffix='.tmp',dir=a.output.parent);os.close(fd);temporary=Path(tmp)
 try:
  subprocess.run(['cp','--reflink=auto',str(a.source),str(temporary)],check=True)
  install_kernel(temporary,a.kernel_dir)
  editor=Editor(temporary)
  remove_old_libraries(editor)
  for destination,source in files.items():editor.mkdir(str(Path(destination).parent));editor.update(destination,source.read_bytes())
  editor.finish()
  with temporary.open('rb') as stream:os.fsync(stream.fileno())
  os.replace(temporary,a.output)
  directory_fd=os.open(a.output.parent,os.O_DIRECTORY)
  try:os.fsync(directory_fd)
  finally:os.close(directory_fd)
 finally:temporary.unlink(missing_ok=True)
 print('Installed bootable LBS image:',a.output)
if __name__=='__main__':main()
