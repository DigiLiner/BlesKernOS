#!/usr/bin/env python3
"""Enable/build Wine's OSS driver using its configured i386 build directory."""
import argparse, os, shlex, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--jobs',type=int,default=4)
a=p.parse_args(); root=Path(__file__).resolve().parents[3]
obj=a.build_dir.resolve(); source=a.source.resolve()
args=shlex.split(subprocess.check_output([str(obj/'config.status'),'--config'],text=True))
args=[x for x in args if not x.startswith(('CPPFLAGS=','--without-oss','--with-oss'))]
headers=obj/'.bles-oss-headers'
if not headers.exists(): headers.symlink_to(root/'ports/wine/audio/include',target_is_directory=True)
args+=['--with-oss','CPPFLAGS=-I'+str(headers)]
env=os.environ.copy()
# Wine's assembler must generate 32-bit objects even without a MinGW toolchain.
import sys
sys.path.insert(0,str(root/'ports/wine/scripts'))
from toolchain32 import prepare
work=obj/'.bles-audio-toolchain'; work.mkdir(parents=True,exist_ok=True)
prepare(work,env)
subprocess.run([str(source/'configure'),*args],cwd=obj,env=env,check=True)
# Relink only these modules with guest-visible library search paths.
for name in ['wineoss.drv.so','wineoss.so']:
    (obj/'dlls/wineoss.drv'/name).unlink(missing_ok=True)
subprocess.run(['make',f'-j{a.jobs}',r'LDFLAGS=-Wl,-rpath,\$$ORIGIN:/WINEGATE/lib',
                'dlls/wineoss.drv/all'],cwd=obj,env=env,check=True)
