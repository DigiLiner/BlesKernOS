#!/usr/bin/env python3
"""Validate actual ELF dependency graphs against the packaged i386 GPU runtime."""
import importlib.util,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('gallium_builder',root/'ports/wine/graphics/build-gallium.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
lib=root/'ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib'
closure=builder.validate_runtime_closure(lib/'libbkvirgl.so.8',lib)
assert 'libbkvirgl.so.8' in closure and 'libstdc++.so.6' in closure
with tempfile.TemporaryDirectory(prefix='bk-gpu-package-') as directory:
 d=Path(directory);(d/'dep.c').write_text('int helper(void){return 7;}\n')
 (d/'gpu.c').write_text('extern int helper(void);int gpu(void){return helper();}\n')
 subprocess.run(['gcc','-m32','-shared','-fPIC',str(d/'dep.c'),'-Wl,-soname,libhelper.so.1','-o',str(d/'libhelper.so.1')],check=True)
 subprocess.run(['gcc','-m32','-shared','-fPIC',str(d/'gpu.c'),'-L'+str(d),'-l:libhelper.so.1','-o',str(d/'gpu.so')],check=True)
 assert builder.validate_runtime_closure(d/'gpu.so',d)==['gpu.so','libhelper.so.1']
 (d/'libhelper.so.1').unlink()
 try:builder.validate_runtime_closure(d/'gpu.so',d)
 except RuntimeError as e:assert 'gpu.so -> libhelper.so.1' in str(e)
 else:raise AssertionError('missing dependency accepted')
 subprocess.run(['gcc','-shared','-fPIC',str(d/'dep.c'),'-o',str(d/'host.so')],check=True)
 try:builder.validate_runtime_closure(d/'host.so',d)
 except RuntimeError as e:assert 'not ELF i386' in str(e)
 else:raise AssertionError('host architecture accepted')
print('PASS: actual packaged GPU closure is i386 and complete; missing dependencies and host ELF rejected')
