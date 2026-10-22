#!/usr/bin/env python3
"""Port original Mesa demos 9.0.0 scenes to native ELF32/Gallium/Mesa 3.5."""
from pathlib import Path
import hashlib,json,shutil,subprocess,sys,tarfile,urllib.request
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/native-mesa-demos'
SOURCE=WORK/'mesa-demos-9.0.0'
PACKAGE=ROOT/'build/native-gallium/package'
RUNTIME=PACKAGE/'SYSTEM/LIBS/GALLIUM'
SHA='3046a3d26a7b051af7ebdd257a5f23bfeb160cad6ed952329cdff1e9f1ed496b'
def run(args):subprocess.run([str(x) for x in args],check=True)
def main():
    WORK.mkdir(parents=True,exist_ok=True)
    archive=WORK/'mesa-demos-9.0.0.tar.xz'
    if not archive.exists():urllib.request.urlretrieve('https://archive.mesa3d.org/demos/mesa-demos-9.0.0.tar.xz',archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA:raise RuntimeError('Mesa demos source checksum')
    if not SOURCE.exists():
        with tarfile.open(archive) as tar:tar.extractall(WORK,filter='data')
    run(['make','-j3','build/system/libs/native/NGS3D.LBS','build/tests/userland/LBSTRI.BEX'])
    shutil.copyfile(ROOT/'build/system/libs/native/NGS3D.LBS',PACKAGE/'SYSTEM/NGS3D.LBS')
    shutil.copyfile(ROOT/'build/tests/userland/LBSTRI.BEX',PACKAGE/'SYSTEM/PROGRAMS/LBSTRI.BEX')
    run([sys.executable,ROOT/'ports/wine/graphics/build-mesa.py'])
    shutil.copyfile(ROOT/'build/wine/graphics/libbkmesa.so.1',RUNTIME/'libbkmesa.so.1')
    link=['gcc','-m32','-Wl,--disable-new-dtags','-Wl,-rpath,/SYSTEM/LIBS/GALLIUM','-Wl,-rpath-link,'+str(RUNTIME),'-L'+str(RUNTIME)]
    run([*link,'-O2','-fPIC','-shared','-Wl,-z,defs','-Wl,-soname,libbkgmesa.so.1','-I'+str(ROOT/'libs/Mesa-3.5/include'),ROOT/'ports/mesa-demos/classic-client.c','-l:libbkmesa.so.1','-ldl','-o',RUNTIME/'libbkgmesa.so.1'])
    includes=[ROOT/'ports/mesa-demos/include',SOURCE/'src/glad/include',SOURCE/'src/util',ROOT/'libs/Mesa-3.5/include']
    flags=['gcc','-m32','-march=pentium3','-mfpmath=387','-O2','-fno-strict-aliasing',*['-I'+str(p) for p in includes],'-include',SOURCE/'src/glad/include/glad/glad.h','-DDEMOS_DATA_DIR="/SYSTEM/SHARE/MESADEMOS/"','-ffunction-sections','-fdata-sections']
    def compile(p,extra=[]):
        obj=WORK/(p.name+'.o')
        run([*flags,*extra,'-c',p,'-o',obj]);return obj
    glad=WORK/'glad.o'
    run(['gcc','-m32','-O2','-I'+str(SOURCE/'src/glad/include'),'-DgladLoadGL=gladLoadGLPlatform','-c',SOURCE/'src/glad/src/glad.c','-o',glad])
    common=[glad,compile(ROOT/'ports/mesa-demos/glut-native.c'),compile(ROOT/'ports/mesa-demos/vendor/glut_teapot.c')]
    glu=[compile(ROOT/'libs/Mesa-3.5/src-glu'/name) for name in ['glu.c','project.c','quadric.c','mipmap.c']]
    utility=[compile(SOURCE/'src/util'/name) for name in ['readtex.c','trackball.c']]
    binaries=[]
    for name in ['gears','gloss','morph3d']:
        obj=compile(SOURCE/'src/demos'/(name+'.c'),['-Dmain=bkg_demo_main'])
        out=PACKAGE/'SYSTEM/PROGRAMS'/('MD'+name.upper()+'.ELF')
        run([*link,'-Wl,--gc-sections','-Wl,--dynamic-linker=/SYSTEM/LIBS/GALLIUM/ld-linux.so.2',obj,*common,*glu,*utility,'-ldl','-lm','-o',out])
        run(['strip','--strip-unneeded',out]);binaries.append(out)
        run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),'-DMESA_DEMO="'+name.upper()+'"','-c',ROOT/'ports/mesa-demos/launcher.c','-o',out.with_suffix('.BEX')])
    data=PACKAGE/'SYSTEM/SHARE/MESADEMOS';data.mkdir(parents=True,exist_ok=True)
    for name in ['reflect.rgb','tile.rgb']:shutil.copyfile(SOURCE/'src/data'/name,data/name)
    licenses=PACKAGE/'SYSTEM/SHARE/LICENSES/MESADEMOS';licenses.mkdir(parents=True,exist_ok=True)
    for name in ['COPYING']:
        if (SOURCE/name).exists():shutil.copyfile(SOURCE/name,licenses/name)
    for name in ['gears','gloss','morph3d']:shutil.copyfile(SOURCE/'src/demos'/(name+'.c'),licenses/(name+'-SOURCE.c'))
    shutil.copyfile(ROOT/'ports/mesa-demos/vendor/glut_teapot.c',licenses/'GLUT-TEAPOT.c')
    for name in ['docs/COPYING','docs/COPYRIGHT','LICENSE']:
        f=ROOT/'libs/Mesa-3.5'/name
        if f.exists():shutil.copyfile(f,licenses/('MESA35-'+f.name))
    run([sys.executable,ROOT/'ports/mesa-demos/package-sources.py'])
    # Reuse the dependency closure resolver, then refresh the shared manifest.
    sys.path.insert(0,str(ROOT/'ports/gallium'));import build as gallium
    deps=set()
    for binary in [*binaries,RUNTIME/'libbkgmesa.so.1']:deps.update(gallium.package_closure(binary))
    shutil.copyfile(ROOT/'ports/gallium/include/bleskernos_gallium.h',PACKAGE/'SYSTEM/SDK/GALLIUM/INCLUDE/bleskernos_gallium.h')
    manifest_path=ROOT/'build/native-gallium/manifest.json'
    manifest=json.loads(manifest_path.read_text())
    manifest['mesa_demos']={'version':'9.0.0','sha256':SHA,'scenes':['gears','gloss','morph3d'],'scene_changes':False}
    manifest['dependencies']=sorted(set(manifest['dependencies'])|deps)
    manifest['files']={str(f.relative_to(PACKAGE)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(PACKAGE.rglob('*')) if f.is_file()}
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    print('Mesa demos native package ready')
if __name__=='__main__':main()
