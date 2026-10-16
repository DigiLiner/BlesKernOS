#!/usr/bin/env python3
"""Build the existing Mesa/GFX3D rasterizer for the Linux ELF Wine runtime."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess
ROOT=Path(__file__).resolve().parents[3]
mesa=ROOT/'libs/Mesa-3.5'
out=ROOT/'build/wine/graphics'
out.mkdir(parents=True,exist_ok=True)
sources=[]
for directory in ['src','src/math','src/swrast','src/swrast_setup','src/tnl','src/array_cache','src/BlesKernOS']:
    sources += [p for p in sorted((mesa/directory).glob('*.c'))
                if p.name not in ('config.c','CONFIG.c') and not p.name.startswith('m_debug_')]
sources += [mesa/'src/OSmesa/osmesa.c',mesa/'bleskernos/config_stub.c',ROOT/'ports/wine/graphics/gfx3d-linux.c']
flags=['gcc','-m32','-march=pentium3','-O2','-fPIC','-fno-strict-aliasing','-DMESA_BLESKERNOS=1','-DPTHREADS','-D_REENTRANT']
flags += ['-I'+str(mesa/d) for d in ['include','src','src/math','src/swrast','src/swrast_setup','src/tnl','src/array_cache','src/BlesKernOS']]
def compile(p):
    obj=out/(str(p.relative_to(ROOT)).replace('/','_')+'.o')
    if not obj.exists() or p.stat().st_mtime>obj.stat().st_mtime:
        subprocess.run(flags+['-c',str(p),'-o',str(obj)],check=True)
    return str(obj)
with ThreadPoolExecutor(max_workers=4) as pool: objects=list(pool.map(compile,sources))
subprocess.run(['gcc','-m32','-shared','-Wl,-Bsymbolic','-Wl,-z,defs','-Wl,-soname,libbkmesa.so.1',*objects,'-lm','-lpthread','-o',str(out/'libbkmesa.so.1')],check=True)
print(out/'libbkmesa.so.1')
