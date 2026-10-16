"""Stage updated public C SDK files without modifying installed OS images."""
from pathlib import Path
import hashlib
import shutil
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/libc-update'
include=out/'SYSTEM/SDK/INCLUDE'
lib=out/'SYSTEM/SDK/LIB'
examples=out/'SYSTEM/SDK/EXAMPLES/LIBCTEST'
for directory in [include,lib,examples]:directory.mkdir(parents=True,exist_ok=True)
for name in ['stdlib.h','string.h','stdint.h','inttypes.h','limits.h']:
    shutil.copyfile(ROOT/'sdk/include'/name,include/name.upper())
shutil.copyfile(ROOT/'build/sdk/libblesk_posix.a',lib/'LIBPOSIX.A')
shutil.copyfile(ROOT/'sdk/examples/libc_regression.c',examples/'LIBCTEST.C')
native_test=ROOT/'build/libc-tests/LIBCTEST.BEX'
if native_test.exists():shutil.copyfile(native_test,examples/'LIBCTEST.BEX')
(examples/'Makefile').write_text('all:\n\tcompile -x c -I/SYSTEM/SDK/INCLUDE LIBCTEST.C -o LIBCTEST.O\n\tlink LIBCTEST.O /SYSTEM/SDK/LIB/LIBPOSIX.A /SYSTEM/SDK/LIB/LIBBLESK.A -o LIBCTEST.BEX\n')
(out/'README.txt').write_text(
    'Actualizacion de libc del SDK de BlesKernOS 0.9.\n'
    'Copiar el contenido de SYSTEM sobre /SYSTEM de una instalacion con SDK completo.\n'
    'Conservar una copia anterior de LIBPOSIX.A y los cinco headers reemplazados.\n'
    'No cambia el kernel ni los binarios ya compilados: recompilar aplicaciones\n'
    'para usar las funciones nuevas. Prueba: cd /SYSTEM/SDK/EXAMPLES/LIBCTEST;\n'
    'ejecutar make y luego start LIBCTEST.BEX (comandos separados).\n'
    'El source y las pruebas estan en sdk/lib/posix_stdlib.c, posix_string.c,\n'
    'sdk/examples/libc_regression.c y tests/sdk/test_sdk_libc.py.\n')
(out/'SHA256SUMS.txt').write_text(''.join(
    hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(out).as_posix()+'\n'
    for p in sorted((out/'SYSTEM').rglob('*')) if p.is_file()))
print('SDK libc update:',out)
print('ZIP:',shutil.make_archive(str(ROOT/'build/libc-update'),'zip',out))
