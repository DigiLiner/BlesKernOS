#!/usr/bin/env python3
"""Ship GLU sources, relinkable demo objects and third-party notices."""
from pathlib import Path
import shutil
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/native-mesa-demos'
UPSTREAM=WORK/'mesa-demos-9.0.0'
PACKAGE=ROOT/'build/native-gallium/package'
def package_sources():
    sdk=PACKAGE/'SYSTEM/SDK/MESADEMOS';objects=sdk/'OBJECTS';objects.mkdir(parents=True,exist_ok=True)
    for p in WORK.glob('*.o'):shutil.copyfile(p,objects/p.name)
    glu=sdk/'GLU';glu.mkdir(exist_ok=True)
    source=ROOT/'libs/Mesa-3.5/src-glu'
    for p in [*(source.glob('*.h')),*(source/name for name in ['glu.c','project.c','quadric.c','mipmap.c'])]:shutil.copyfile(p,glu/p.name)
    include=sdk/'INCLUDE/GL';include.mkdir(parents=True,exist_ok=True)
    for name in ['gl.h','glu.h']:shutil.copyfile(ROOT/'libs/Mesa-3.5/include/GL'/name,include/name)
    shutil.copytree(UPSTREAM/'src/glad/include',sdk/'INCLUDE',dirs_exist_ok=True)
    notices=PACKAGE/'SYSTEM/SHARE/LICENSES/MESADEMOS'
    for name in ['trackball.c','readtex.c']:shutil.copyfile(UPSTREAM/'src/util'/name,notices/name)
    shutil.copyfile(UPSTREAM/'src/glad/src/glad.c',notices/'GLAD-SOURCE.c')
    shutil.copyfile('/usr/share/common-licenses/LGPL-2',notices/'COPYING.LIB')
    (sdk/'RELINK.TXT').write_text('''Mesa demos / GLU relinking materials

The selected Mesa 3.5 GLU sources are LGPL 2 or later. Their original notices
and COPYING.LIB are included. OBJECTS contains the application objects and
original GLU objects used for the native ELF32 programs. The original scene
notices are in /SYSTEM/SHARE/LICENSES/MESADEMOS.

On a host with GCC/binutils and 32-bit GNU C development libraries, from this
directory, recompile modified GLU sources, for example:

gcc -m32 -O2 -fPIC -IINCLUDE -IGLU -include INCLUDE/glad/glad.h -c GLU/quadric.c -o OBJECTS/quadric.c.o

Then relink a demo (replace gears with gloss or morph3d as appropriate):

gcc -m32 -Wl,--gc-sections -Wl,--disable-new-dtags -Wl,-rpath,/SYSTEM/LIBS/GALLIUM -Wl,--dynamic-linker=/SYSTEM/LIBS/GALLIUM/ld-linux.so.2 OBJECTS/gears.c.o OBJECTS/glad.o OBJECTS/glut-native.c.o OBJECTS/glut_teapot.c.o OBJECTS/glu.c.o OBJECTS/project.c.o OBJECTS/quadric.c.o OBJECTS/mipmap.c.o OBJECTS/readtex.c.o OBJECTS/trackball.c.o -ldl -lm -o MDGEARS.ELF

The resulting ELF uses the installed OS native graphics client and runtime.
The complete port/build recipes and application adapter sources are in the
workspace under ports/mesa-demos; the pinned upstream source is the official
mesa-demos-9.0.0 archive (SHA256 recorded in build.py and the package manifest).
''')
if __name__=='__main__':package_sources()
