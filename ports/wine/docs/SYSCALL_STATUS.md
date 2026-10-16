# Cobertura Linux i386

Auditoria de codigo y pruebas glibc 2.39 / Wine 11.0. Regenerar con `python3 ports/wine/scripts/audit-syscalls.py`.

`IMPLEMENTED`: operacion acotada implementada. `PARTIAL`: cobertura incompleta.
`MISSING`: ENOSYS. `INCORRECT`: hay codigo, pero no cumple el contrato Linux.
`NOT YET TESTED`: sin evidencia para afirmar conformidad completa. Ninguna fila certifica Wine.

Las llamadas no incluidas en la tabla de traduccion retornan -38; nunca caen en una syscall nativa del mismo numero.

| N.º | Nombre | Estado | Observacion |
|---:|---|---|---|
| 1 | `exit` | PARTIAL | Salida Linux codificada; clear_child_tid y robust futex al terminar la tarea. |
| 2 | `fork` | MISSING | Sin traduccion Linux i386. |
| 3 | `read` | PARTIAL | VFS, consola, pipes y sockets Unix; limite de 64 KiB por llamada. |
| 4 | `write` | PARTIAL | VFS, consola e IPC; backpressure, EOF y EPIPE/SIGPIPE. Limite de 64 KiB. |
| 5 | `open` | PARTIAL | Mismo adaptador que openat; flags desconocidos rechazados. O_APPEND y permisos POSIX pendientes. |
| 6 | `close` | PARTIAL | Archivos reales; cerrar consola sin redireccion no se modela como Linux. |
| 7 | `waitpid` | PARTIAL | Bloqueo, WNOHANG, ECHILD, EINTR y estado Linux; sin grupos ni WUNTRACED/WCONTINUED. |
| 10 | `unlink` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 11 | `execve` | PARTIAL | Reemplaza imagen conservando PID, argv/envp, FD y mascara; aplica CLOEXEC. Solo proceso sin otros hilos. |
| 12 | `chdir` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 13 | `time` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 19 | `lseek` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 20 | `getpid` | IMPLEMENTED | Devuelve ID de proceso. |
| 24 | `getuid` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 33 | `access` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 37 | `kill` | PARTIAL | PID/TID real, SIG_DFL/SIG_IGN y cola acotada; sin grupos ni control de trabajos; STOP por defecto devuelve ENOSYS. |
| 38 | `rename` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 39 | `mkdir` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 41 | `dup` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 42 | `pipe` | IMPLEMENTED | Extremos en tabla FD del kernel, EOF, EPIPE y escrituras atomicas hasta PIPE_BUF=4096. |
| 45 | `brk` | PARTIAL | Reserva fisica contigua de 4 MiB, no al final de la imagen. |
| 47 | `getgid` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 49 | `geteuid` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 50 | `getegid` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 54 | `ioctl` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 55 | `fcntl` | PARTIAL | FD flags, dup, GETFL/SETFL y locks POSIX 32-bit; sin OFD/flock ni O_APPEND. |
| 60 | `umask` | INCORRECT | Devuelve 0022 sin guardar la mascara. |
| 63 | `dup2` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 64 | `getppid` | IMPLEMENTED | Relacion padre conservada durante fork y exec, comprobada en casos 21 y 22. |
| 65 | `getpgrp` | INCORRECT | Usa ID de proceso, no grupo independiente. |
| 67 | `sigaction` | MISSING | Sin traduccion Linux i386. |
| 76 | `getrlimit` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 78 | `gettimeofday` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 82 | `select` | MISSING | Sin traduccion Linux i386. |
| 83 | `symlink` | MISSING | Sin traduccion Linux i386. |
| 85 | `readlink` | PARTIAL | FAT/ISO sin symlinks: EINVAL en nodo existente, ENOENT en ausente. Desbloquea realpath y Wine W1. Sin /proc/self/exe. |
| 90 | `mmap` | PARTIAL | old_mmap con offset en bytes alineado; mismos limites que mmap2. |
| 91 | `munmap` | PARTIAL | Solo reserva completa exacta; no divide ni tolera huecos como Linux. |
| 93 | `ftruncate` | PARTIAL | Redimensiona archivos VFS de hasta 31 bits; cambios de tamano mientras estan mapeados se rechazan. |
| 102 | `socketcall` | PARTIAL | AF_UNIX SOCK_STREAM: socket/pair/bind/listen/connect/accept/name/peer/shutdown/send/recv/msg. Sin AF_INET ni datagramas Linux. |
| 114 | `wait4` | PARTIAL | Misma espera que waitpid; rusage no nulo devuelve ENOSYS. |
| 116 | `sysinfo` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 118 | `fsync` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 119 | `sigreturn` | PARTIAL | Frame antiguo i386 y restauracion de contexto/FPU; API sigaction antigua aun ausente. |
| 120 | `clone` | PARTIAL | Threads VM/FS/FILES/SIGHAND/THREAD o proceso privado SIGCHLD; valida TLS/TID/flags y publica hijo al final. No vfork ni combinaciones compartidas intermedias. |
| 122 | `uname` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 125 | `mprotect` | PARTIAL | READ/WRITE/NONE y RELRO main/interprete; sin NX real en este paging i386. |
| 132 | `getpgid` | INCORRECT | No valida PID ni administra grupos reales. |
| 140 | `_llseek` | PARTIAL | Resultado de 64 bits y cookies de directorio; VFS limita offsets a entero positivo de 31 bits. |
| 141 | `getdents` | MISSING | Sin traduccion Linux i386. |
| 142 | `_newselect` | PARTIAL | Conjuntos FD, readiness, timeout y EBADF; esperas mediante scheduler. |
| 144 | `msync` | PARTIAL | Writeback FAT de objetos compartidos y validacion de rango/flags; aliases y read/write coherentes. No hay cache de disco asincrona independiente. |
| 145 | `readv` | MISSING | Sin traduccion Linux i386. |
| 146 | `writev` | PARTIAL | iovec i386 validado, <=1024, escritura por fragmentos; regular/consola, sin atomicidad de pipes. |
| 158 | `sched_yield` | IMPLEMENTED | Usa scheduler existente; conserva registros de la tarea llamante. |
| 162 | `nanosleep` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 168 | `poll` | PARTIAL | Readiness, timeout, EINTR, HUP/ERR/NVAL para los FD implementados; sin epoll. |
| 173 | `rt_sigreturn` | PARTIAL | Frame i386, ucontext, registros, mascara, altstack, x87/XMM y validacion de selectores/EFLAGS. |
| 174 | `rt_sigaction` | PARTIAL | 64 acciones compartidas por threads, SIGINFO/ONSTACK/RESTORER/NODEFER/RESETHAND; SA_RESTART y job control pendientes. |
| 175 | `rt_sigprocmask` | IMPLEMENTED | Mascara de 64 bits por hilo, block/unblock/set y SIGKILL/SIGSTOP no bloqueables. |
| 176 | `rt_sigpending` | PARTIAL | Consulta de pendientes bloqueadas de la cola por hilo; sin cola compartida de proceso. |
| 179 | `rt_sigsuspend` | MISSING | Sin traduccion Linux i386. |
| 183 | `getcwd` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 186 | `sigaltstack` | PARTIAL | Altstack por hilo, limites y EPERM en handler activo. Sin SS_AUTODISARM. |
| 191 | `ugetrlimit` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 192 | `mmap2` | PARTIAL | 8 reservas y 64 MiB; MAP_PRIVATE y MAP_SHARED anonimo/archivo con aliases y fork. Compartido: tamano fijo, sin MAP_FIXED ni paginas mas alla de EOF. Caso 28. |
| 194 | `ftruncate64` | PARTIAL | ABI i386 de 64 bits; VFS solo admite tamanos positivos de 31 bits. |
| 195 | `stat64` | PARTIAL | Identidad FAT por volumen/registro, ID estable durante rename; atime/mtime FAT reales y ctime aproximado con mtime. ISO usa extent. |
| 196 | `lstat64` | PARTIAL | Igual a stat64 sobre FAT/ISO, que no admiten symlinks. Nodos Unix en RAM reconocidos. |
| 197 | `fstat64` | PARTIAL | Identidad conservada por FD, directorios, pipes, sockets y consola diferenciados. Sin metadatos POSIX completos. |
| 199 | `getuid32` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 200 | `getgid32` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 201 | `geteuid32` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 202 | `getegid32` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 219 | `madvise` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 220 | `getdents64` | PARTIAL | Formato Linux variable alineado a 8, tipos, inode coherente y cookies compartidas por dup/fork. VFS limita listados a 8192 entradas. |
| 221 | `fcntl64` | PARTIAL | Locks POSIX 32/64-bit GETLK/SETLK/SETLKW, conflictos, rangos, split, close/exit/CLOEXEC; 128 intervalos. Caso 29. |
| 224 | `gettid` | IMPLEMENTED | Devuelve ID de tarea. |
| 238 | `tkill` | PARTIAL | Entrega dirigida a TID Linux; comparte limites de kill. |
| 240 | `futex` | PARTIAL | WAIT/WAKE y BITSET atomicos, timeout, EINTR, private/shared key y cuenta real de despertares. Sin REQUEUE ni PI. |
| 241 | `sched_setaffinity` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 242 | `sched_getaffinity` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 243 | `set_thread_area` | PARTIAL | Un descriptor TLS (GDT 6); glibc inicializa GS. No son las tres entradas Linux. |
| 244 | `get_thread_area` | PARTIAL | Solo descriptor GDT 6. |
| 252 | `exit_group` | PARTIAL | Solicita terminar el grupo. Robust lists de otros hilos y concurrencia con exec requieren ampliar pruebas. |
| 254 | `epoll_create` | MISSING | Sin traduccion Linux i386. |
| 255 | `epoll_ctl` | MISSING | Sin traduccion Linux i386. |
| 256 | `epoll_wait` | MISSING | Sin traduccion Linux i386. |
| 258 | `set_tid_address` | IMPLEMENTED | Guarda clear_child_tid, escribe cero y despierta waiter al salir. |
| 265 | `clock_gettime` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 266 | `clock_getres` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 267 | `clock_nanosleep` | PARTIAL | Solo relativo; traduccion historica no valida reloj; sin interrupcion por senales. |
| 270 | `tgkill` | PARTIAL | Valida pertenencia TID/TGID y entrega dirigida; comparte limites de kill. |
| 295 | `openat` | PARTIAL | Directorios, dirfd relativo, O_DIRECTORY, O_NOFOLLOW sin symlinks, O_EXCL y CLOEXEC. Sin O_APPEND. |
| 300 | `fstatat64` | PARTIAL | Dirfd relativo/AT_FDCWD/absoluto y AT_EMPTY_PATH; identidad coherente con fstat y getdents64. |
| 305 | `readlinkat` | PARTIAL | Mismo contrato que readlink, dirfd relativo/AT_FDCWD y validacion. Sin O_PATH ni symlinks. |
| 308 | `pselect6` | PARTIAL | Select con timespec y mascara temporal de 64 bits; revisar atomicidad SMP de entrega de senales. |
| 309 | `ppoll` | MISSING | Sin traduccion Linux i386. |
| 311 | `set_robust_list` | PARTIAL | Registra cabeza por hilo y recorre lista acotada al salir; OWNER_DIED, pending y wake. Sin robust PI. |
| 312 | `get_robust_list` | PARTIAL | Consulta cabeza y longitud i386 por TID; proceso actual o tarea Linux existente. |
| 319 | `epoll_pwait` | MISSING | Sin traduccion Linux i386. |
| 327 | `signalfd4` | MISSING | Sin traduccion Linux i386. |
| 328 | `eventfd2` | MISSING | Sin traduccion Linux i386. |
| 329 | `epoll_create1` | MISSING | Sin traduccion Linux i386. |
| 330 | `dup3` | MISSING | Sin traduccion Linux i386. |
| 331 | `pipe2` | IMPLEMENTED | Pipe con NONBLOCK y CLOEXEC, herencia y referencias reales. |
| 340 | `prlimit64` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 355 | `getrandom` | INCORRECT | Reutiliza RDRAND si existe; fallback historico sin entropia criptografica. |
| 359 | `socket` | PARTIAL | AF_UNIX SOCK_STREAM, CLOEXEC y NONBLOCK. |
| 360 | `socketpair` | PARTIAL | Extremos con referencias, herencia y estado compartido. Solo AF_UNIX stream. |
| 361 | `bind` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 362 | `connect` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 363 | `listen` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 364 | `accept4` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 365 | `getsockopt` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 366 | `setsockopt` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 367 | `getsockname` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 368 | `getpeername` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 369 | `sendto` | PARTIAL | Traduccion presente; semantica completa NOT YET TESTED. |
| 370 | `sendmsg` | PARTIAL | iovec i386 y SCM_RIGHTS, referencia retenida y recolector de ciclos. Hasta 8 FD por mensaje. Sin SCM_CREDENTIALS. |
| 371 | `recvfrom` | PARTIAL | Lectura de stream Unix sin direccion de origen; otros modos devuelven error. |
| 372 | `recvmsg` | PARTIAL | SCM_RIGHTS, CMSG_CLOEXEC, CTRUNC y cierre de sobrantes; sin PEEK/WAITALL/CREDENTIALS. |
| 373 | `shutdown` | PARTIAL | Cierre de lectura/escritura de stream; revisar todas las combinaciones de poll/HUP. |
| 383 | `statx` | MISSING | glibc 2.39 cae a fstatat64 tras ENOSYS. |
| 386 | `rseq` | MISSING | glibc 2.39 tolera ENOSYS en este bootstrap. |
| 403 | `clock_gettime64` | MISSING | glibc cae a clock_gettime de 32 bits en pruebas anteriores a 2038. |
| 407 | `clock_nanosleep_time64` | MISSING | Sin traduccion Linux i386. |
| 413 | `pselect6_time64` | MISSING | Sin traduccion Linux i386. |
| 422 | `futex_time64` | MISSING | Sin traduccion Linux i386. |
| 435 | `clone3` | MISSING | Sin traduccion Linux i386. |
| 436 | `close_range` | MISSING | Sin traduccion Linux i386. |
| 449 | `futex_waitv` | MISSING | Sin traduccion Linux i386. |

Ver [HOST_STATUS.md](HOST_STATUS.md) para resultados de cada ejecutable y [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) para los bloqueos de Wine.
