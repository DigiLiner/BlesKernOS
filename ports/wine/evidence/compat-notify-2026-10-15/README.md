# Compatibilidad Wine: notificaciones de directorios (2026-10-15)

Se agregó el backend Linux inotify que Wine 11 utiliza para las APIs Windows
`ReadDirectoryChangesW` y `FindFirst/NextChangeNotification`. Permite que editores,
exploradores y otras aplicaciones reciban cambios en archivos sin consultar
periódicamente el directorio. No constituye una validación general de esos programas.

## Implementación

- Syscalls i386 291/292/293/332: init, add_watch, rm_watch e init1.
- Eventos VFS después de operaciones exitosas: crear, abrir, leer, escribir,
  truncar, cerrar, cambiar atributos, borrar y renombrar. Los renombres llevan
  pares MOVED_FROM/MOVED_TO con cookie y actualizan las rutas vigiladas.
- Descriptores integrados con close, dup, fork, CLOEXEC, NONBLOCK, poll/select,
  FIONREAD y esperas bloqueantes interrumpibles. Se reutilizan las tablas existentes.
- Registros completos con nombres alineados, coalescencia, IN_IGNORED,
  IN_ONESHOT e IN_Q_OVERFLOW. No se reserva memoria para las colas si nadie usa inotify.
- Resolución de `/proc/self/fd/N`, incluyendo readlink, stat y sufijos `/..` o
  nombres de hijos, para descripciones VFS vivas. Sigue los renombres y rechaza
  rutas de descriptores cerrados o reemplazados.
- El callback VFS no bloquea ni vuelve a entrar en VFS. La cola se protege con
  linux_io_lock; el despertar de tareas ocurre después de soltarlo. La lectura
  valida y fija previamente las páginas de usuario antes de copiar bajo el lock.

## Evidencia

La imagen anterior tiene kernel SHA256
`9f7be7cb4ba457b951f58df8eae5d6a2337ae252bfeef9effd83e9e9172e2b96`.
La prueba Windows abre el directorio y crea un archivo, pero vence la espera de
su notificación: [baseline](logs/baseline-valid/guest-serial.log).
El caso usa Z: (raíz del guest); una prueba inicial con C: no era válida y se excluyó.

Con el kernel nuevo pasa la [integración Linux y Windows](logs/final-integration/guest-serial.log):

- Linux: flags y errores, poll/FIONREAD, registros completos, buffers inválidos,
  coalescencia, renombres y cookies, borrado, máscaras, oneshot, overflow y recuperación.
- Linux: procfd/stat/readlink/.., renombre de directorios, lectura bloqueante,
  cola compartida por dup/fork, EINTR y 256 ciclos de creación/cierre de instancia.
- Windows: creación, renombre, modificación, borrado, subdirectorio abierto,
  FindFirst/NextChangeNotification y rearme sin señales espurias.
- ASan/UBSan, nativo y -m32: límites exactos de cola, overflow, máscaras,
  descendientes renombrados y despertar después del unlock.
- Regresiones del host: graphics poll, política de escritura VFS, E/S paginada,
  copia de strings y lifetime de páginas de usuario.
- [Notepad GUI](logs/notepad/result.json): 32 pulsaciones detectadas, copiar/pegar
  antes y después de reiniciar Vision, y compositor CPL3: PASS. Se guardan las
  latencias observadas, pero esta ejecución funcional con actividad de disco del
  host no es una comparación controlada de rendimiento respecto de la ronda anterior.

Se corrigió un error del test: Wine conserva filtro/subtree de la primera
solicitud sobre cada handle. Se abre un handle nuevo para la fase recursiva.
El subdirectorio existente se abre después de armar la vigilancia, para que
Wine registre su inode. La prueba no certifica vigilancia de un árbol existente
arbitrario sin abrir sus subdirectorios. Los ejecutables baseline y final difieren
en esa fase posterior; el primer caso que fallaba en baseline conserva su lógica.

También pasó la [batería completa del runtime](logs/runtime/boot.log): ABI,
33/33 casos y Wine --version. Incluye IPC, sockets Unix, fork/exec, archivos,
symlinks, locks, MAP_SHARED, caché privada, futex y madvise.

## Límites explícitos

Hay 64 watches por instancia, una cola de 8192 bytes y un máximo compartido de
128 dispositivos Linux. Al agotarse watches se devuelve ENOSPC; al llenarse la
cola se informa IN_Q_OVERFLOW. Cada watch Linux sólo cubre un nivel: Wine
organiza su propia vigilancia recursiva. Las rutas tienen el límite VFS de 260 bytes.

El productor observa operaciones VFS. No cubre escrituras por mmap ni operaciones
que eviten VFS, desmontajes, equivalencias entre todos los alias de volúmenes,
ni todos los detalles Linux de archivos borrados que permanecen abiertos.
Los eventos de un archivo desvinculado se omiten; su watch se retira al borrarlo.
`/proc/self/fd` es una resolución limitada para descriptores VFS propios; no es
una implementación completa de procfs, lstat de enlaces procfd o descriptores socket.
No se agregaron epoll, .NET, Direct3D ni soporte general de instaladores en esta ronda.

## Repetir

Desde la raíz del proyecto, con QEMU/KVM, gcc multilib y MinGW i686 instalados:

```sh
python3 tests/kernel/run_wine_notify.py \
  --base build/bleskernos-wine-compat.img \
  --out build/wine-notify/prueba-01
```

Usar un directorio de salida nuevo en cada ejecución. El runner compila el kernel
actual y ambos tests, instala fixtures sólo en una copia y ejecuta un guest con
1536 MiB, dos CPU y disco IDE. El criterio es ALL PASS en Linux y Windows, salida
0 y result.json PASS; `--windows-only` omite la batería Linux. El modo
`--expect-failure` sólo sirve para registrar una baseline que efectivamente falla.

Imagen normal: `build/bleskernos-wine-compat.img`, sin launcher ni ejecutables de
prueba agregados. Se conservaron MBR y todos los bytes de la región del sistema
de archivos de la imagen responsive; sólo se actualizaron stage2 y kernel.
[Manifiesto de imagen](logs/image.json), [hashes de fuentes](source-hashes.json),
[patch respecto del estado inicial](changes.patch) y snapshots en `sources/`.
La imagen responsive anterior se conserva intacta.

Referencias: [backend Wine 11](https://github.com/wine-mirror/wine/blob/wine-11.0/server/change.c),
[inotify](https://man7.org/linux/man-pages/man7/inotify.7.html) e
[inotify_add_watch](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html).
