# Limitaciones verificadas y trabajo pendiente

Las pruebas glibc cubren las primitivas implementadas, no la conformidad Linux
completa. W1 (`wine --version`) esta aprobada; W2 sigue bloqueada por permisos. No hay gates aprobadas de wineserver, wineboot, prefix
persistente ni Office 95 sobre esta ruta. Ver los resultados reales en HOST_STATUS.

- Procesos: clone admite threads con recursos compartidos y procesos privados
  SIGCHLD. Faltan vfork, combinaciones intermedias de flags, fork desde un hilo,
  exec con otros hilos activos y grupos de procesos. wait admite WNOHANG, no
  WUNTRACED/WCONTINUED ni rusage. argv/envp: 128 entradas combinadas y 16 KiB.
- Threads: WAIT/WAKE/BITSET, clear_child_tid y robust OWNER_DIED implementados;
  faltan REQUEUE, futex PI y robust PI. TLS usa una sola entrada GDT, no las tres
  de Linux. Salida de todo el grupo requiere ampliar pruebas de robust lists.
- Senales: frames, mascaras, altstack, sigreturn y excepciones implementados.
  Faltan SA_RESTART, sigsuspend/sigtimedwait, SS_AUTODISARM y control de trabajos.
  STOP por defecto se rechaza con ENOSYS, no mata el proceso. Cola de 32 entradas
  por hilo; no existe cola pendiente compartida de proceso ni seleccion completa
  del hilo receptor. Nanosleep no se interrumpe aun con senales.
- IPC: AF_UNIX SOCK_STREAM, pipes, poll/select y SCM_RIGHTS implementados. Limites:
  32 pares, buffers de 4096 bytes, backlog 8, 8 bloques de derechos por extremo y
  8 FD por bloque, 32 nombres pathname. Los nombres viven en RAM hasta unlink.
  Faltan SCM_CREDENTIALS/SO_PASSCRED, datagramas, SEQPACKET, PEEK/WAITALL, epoll,
  eventfd/signalfd y AF_INET Linux. No se reemplazo wineserver por handles Win32.
- VM: ocho reservas por proceso, hasta 64 MiB por llamada y backing fisico
  contiguo. MAP_FIXED solo dentro de una reserva privada; munmap solo exacto.
  MAP_SHARED comparte frames de archivos/anonimos, aliases y fork; 32 objetos globales. Archivos de tamano fijo: sin MAP_FIXED compartido, sin paginas mas alla de EOF ni cambios de tamano mientras estan mapeados. msync y read/write coherentes; unlink conserva el objeto abierto/mapeado. Un fallo de writeback conserva datos en RAM y puede retener recursos. PROT_NONE conserva la reserva sin acceso de
  usuario. Sin NX real en paging
  i386 actual. La vista heredada del heap de usuario sigue ampliamente compartida;
  las imagenes PIE/interprete no tienen aislamiento equivalente a Linux completo.
- VFS: FAT identifica por volumen/registro, conserva el inode al renombrar y
  distingue archivos vacios. Los IDs se asignan en memoria y no se reutilizan
  mientras se ejecuta el kernel; los registros se reciclan tras unlink. ISO usa
  extent y aun puede confundir entradas vacias. FAT expone atime/mtime reales;
  ctime se aproxima con mtime y permisos siguen sinteticos.
  Hay directorios abiertos, *at relativo, getdents64 y cookies; listados limitados
  a 8192 entradas. readlink/readlinkat devuelven EINVAL para nodos normales y ENOENT si faltan; realpath glibc pasa. Faltan symlinks, dosdevices y O_APPEND,
  semantica completa de archivos abiertos eliminados y rename entre directorios.
  O_CREAT con acceso solo lectura, consola cerrada y los errores detallados de
  VFS requieren completar su contrato. Offsets VFS limitados a 31 bits positivos.
- Aleatoriedad: AT_RANDOM reutiliza el generador existente de getrandom. Sin
  RDRAND, su fallback no es criptograficamente seguro y sigue pendiente de reemplazo.
- Locks: F_GETLK/F_SETLK/F_SETLKW, estructuras i386 de 32/64 bits, identidad VFS, rangos/split, close/exit y CLOEXEC. 128 intervalos globales; sin OFD/flock. Deteccion acotada de ciclos con un waiter por proceso; multiples waits simultaneos de hilos requieren ampliar el modelo.
- Build Wine: compilacion/install Wine 11.0 completos con X11/FreeType/Fontconfig. Bootstrap W1 ejecutado; paquete completo instalado todavia sin gate guest. No incluye bibliotecas Microsoft ni soporte 3D/audio validado.
- La nota del interprete didactico requiere reconstruirlo al actualizar imagenes
  antiguas. Las pruebas del runner hacen esa actualizacion solo en su copia.

Siguiente bloqueo real: mkdir(0700) se observa como 0755. Wineserver aborta al validar /tmp/.wine-0. Hace falta definir y persistir metadatos/permisos POSIX; no cambiar la comprobacion de Wine ni fingir chmod exitoso. Evidencia: build/w2-01 y build/w2-permissions-repro.
La inspeccion de Wine 11.0 confirma uso de SCM_RIGHTS, CMSG_CLOEXEC, SO_PASSCRED
y fchdir en `dlls/ntdll/unix/server.c`; las dos primeras primitivas estan presentes.
No aceptar como aprobadas las pruebas que terminan antes de llegar a su operacion.


