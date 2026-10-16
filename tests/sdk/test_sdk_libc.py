"""Run actual i386 libc code, including integer boundaries, without host libc."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
out=ROOT/'build/libc-tests'
out.mkdir(parents=True,exist_ok=True)
command=['gcc','-m32','-O2','-fno-builtin','-fno-pie','-no-pie','-fno-stack-protector',
         '-nostdlib','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-DLIBC_HOST_TEST','-I'+str(ROOT/'sdk/include'),
         str(ROOT/'sdk/examples/libc_regression.c'),str(ROOT/'sdk/lib/posix_string.c'),
         str(ROOT/'sdk/lib/posix_stdlib.c'),str(ROOT/'kernel/libc/gcc_runtime.c'),
         '-o',str(out/'libc-test')]
subprocess.run(command,check=True)
result=subprocess.run([str(out/'libc-test')])
if result.returncode:raise SystemExit(f'libc regression failed at C line {result.returncode}')
print('PASS i386 libc: numeric bounds/signs/endptr, intmax, division, strings, reentrant tokenizer')
