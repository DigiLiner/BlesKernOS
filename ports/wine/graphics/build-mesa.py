#!/usr/bin/env python3
"""Build the existing Mesa/GFX3D rasterizer for the Linux ELF Wine runtime."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import json,subprocess
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
header_mtime=max((ROOT/"ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h").stat().st_mtime,*(p.stat().st_mtime for directory in (mesa/"include",mesa/"src") for p in directory.rglob("*.h")))
def compile(p):
    obj=out/(str(p.relative_to(ROOT)).replace('/','_')+'.o')
    # Geometry/lighting remain CPU work on the legacy driver. SSE1 keeps
    # Pentium III compatibility; GCC's mixed SSE/x87 allocator is avoided.
    # No fast-math/finite-math assumptions are enabled.
    hot=any(directory in p.parts for directory in ('tnl','math','BlesKernOS'))
    optimization=['-O3','-fno-math-errno','-msse','-mno-sse2','-mfpmath=sse'] if hot else []
    command=flags+optimization+['-c',str(p),'-o',str(obj)]
    stamp=obj.with_suffix('.args.json');encoded=json.dumps(command,indent=2)
    if (not obj.exists() or max(p.stat().st_mtime,header_mtime)>obj.stat().st_mtime or
        not stamp.exists() or stamp.read_text()!=encoded):
        subprocess.run(command,check=True)
        stamp.write_text(encoded)
    return str(obj)
with ThreadPoolExecutor(max_workers=4) as pool: objects=list(pool.map(compile,sources))
subprocess.run(['gcc','-m32','-shared','-Wl,-Bsymbolic','-Wl,-z,defs','-Wl,-soname,libbkmesa.so.1',*objects,'-lm','-lpthread','-o',str(out/'libbkmesa.so.1')],check=True)
print(out/'libbkmesa.so.1')
