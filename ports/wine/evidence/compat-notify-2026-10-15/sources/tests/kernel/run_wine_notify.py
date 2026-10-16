#!/usr/bin/env python3
"""Exercise Linux directory watches and Windows directory notifications in a disposable guest."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,default=root/'build/bleskernos-wine-responsive.img')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--windows-only',action='store_true',help='skip the native inotify ABI regression')
p.add_argument('--expect-failure',action='store_true',help='record the unsupported baseline; requires an actual guest test failure')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
image=out/'fixture-base.img'
if image.exists() or (out/'guest').exists():p.error('use a new output directory')
pe=out/'directory-notify.exe'
subprocess.run(['i686-w64-mingw32-gcc','-O2','-Wall','-Wextra','-Werror',str(root/'ports/wine/tests/directory-notify.c'),'-o',str(pe)],check=True)
subprocess.run(['cp','--reflink=auto',str(a.base.resolve()),str(image)],check=True)
e=Editor(image);e.update('/WINEGATE/bin/directory-notify.exe',pe.read_bytes());e.finish()
try:
 cmd=[sys.executable,str(root/'tests/kernel/run_futex_bench.py'),'--base',str(image),'--out',str(out/'guest'),'--launch-delay','10']
 if not a.windows_only:cmd+=['--program','inotify']
 cmd+=['--program','directory-notify-launch']
 with (out/'runner.log').open('w') as log:r=subprocess.run(cmd,cwd=root,stdout=log,stderr=subprocess.STDOUT)
 serial=(out/'guest/serial.log').read_text(errors='replace') if (out/'guest/serial.log').exists() else ''
 passed=r.returncode==0 and '[WIN-NOTIFY] ALL PASS' in serial
 observed_failure='[WIN-NOTIFY] directory-open PASS' in serial and 'FAIL' in serial and '[FUTEX-SUITE] FAIL' in serial
 result={'result':'PASS' if passed else 'FAIL','expected_failure':a.expect_failure,'observed_guest_failure':observed_failure,
         'pe_sha256':hashlib.sha256(pe.read_bytes()).hexdigest(),'native_test':not a.windows_only,
         'source_kernel_sha256':hashlib.sha256((root/'build/kernel.bin').read_bytes()).hexdigest()}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
 if a.expect_failure:assert observed_failure and not passed,'baseline did not reach the expected guest failure; see runner.log'
 else:assert passed,'directory notification regression failed; see guest/serial.log and runner.log'
finally:image.unlink(missing_ok=True)
