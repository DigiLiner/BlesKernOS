# Organización del kernel y retirada de Win32 propio

Se movieron 66 archivos del kernel a carpetas por subsistema. La raíz de
`kernel/` conserva solo su README; las interfaces internas siguen en
`kernel/include/` y los headers estándar están en `kernel/libc/include/`.
Se actualizaron includes, reglas de build, herramientas, pruebas y referencias.

Se eliminaron `kernel/win32/`, los cargadores PE/NE, sus headers, la biblioteca
propia `WINE.A`, el proveedor `OLESTG.DVR`, pruebas, utilidades de validación
y documentación de esa implementación. Las builds User y Developer ya no
compilan, enlazan ni empaquetan esa capa. El actualizador incremental también
retira sus antiguas bibliotecas del disco al ejecutarse.

Windows se ejecuta exclusivamente a través de Wine 11 sobre la ABI Linux
existente. `WINE.BEX` ya no ofrece el modo nativo anterior; el nuevo
`wine-tool.ELF` permite ejecutar las herramientas Windows por consola y
propagar su estado de salida. Los slots 7 y 33 de syscalls quedan reservados,
sin cambiar los números de la ABI nativa. Un byte reservado conserva el
layout de tareas que pueden consultar módulos nativos ya instalados. Se mantienen las reservas de memoria
que Wine necesita para sus imágenes de base fija y la validación general de
retornos de APIs, incluidos los retornos de structs de GCC i386.

Se borraron 38 patches históricos y cuatro backups. Se conservaron los diez
patches consumidos por Wine, Mesa y TuxRacer y los ocho de las dependencias de
Freeciv. Estos últimos forman parte de sus inputs de build/distribución.
Las dependencias externas y los runtimes Wine se mantienen completos.

## Verificación

- Compilación x86 del kernel y de los objetos User/Developer, drivers y SDK:
  correcta. El kernel resultante ocupa unos 776 KB.
- Pruebas reales de ABI i386, parser del lanzador Wine, reserva del kernel,
  libc y archivos grandes BFS4/VFS: correctas.
- Planes de build User, Developer y Raspberry Pi: rutas resueltas.
- Arranque QEMU con 2 GB, SMP y VirtIO: Vision y warmup Wine correctos;
  escritorio y terminal visibles. NASM respondió por la ruta nueva y devolvió 0.
- Harness en una copia temporal: NASM, GNU Make y ld devolvieron 0; TinyCC
  compiló HELLO.C y se comprobó también el retorno de error 1 del compilador.
  [Evidencias y condiciones del harness](../../ports/wine/evidence/kernel-cleanup-2026-10-08/result.json).
- Con 256/768 MB se observaron fallos ENOMEM durante forks de Wine. El mismo
  problema se reprodujo con la imagen anterior a esta retirada a 768 MB.
- Raspberry Pi llega al enlace, donde faltan símbolos AEABI, TinyGL y APIs.
  La build ARM completa no está validada.

## Límites pendientes

El self-hosting completo dentro del invitado requiere adaptar sus recetas:
el GNU Make Windows anterior dependía de un intérprete propio que podía lanzar
comandos BEX. Ejecutar las herramientas con Wine no sustituye ese intérprete.
El estado se detalla en [self-hosting](self-hosting.md).

No se reconstruyeron las imágenes de producción. Las pruebas utilizaron una
copia temporal; `build/bleskernos-ata-user.img` conserva su SHA-256 previo.
Los cambios se incorporan al generar las siguientes imágenes o al ejecutar
`make user-update`, que conserva los archivos del usuario.

[Inventario de movimientos, eliminaciones y validación](kernel-win32-removal-2026-10-08.json).
[Guía del kernel](../../kernel/README.md).
