# Estado del host y evidencia

## Fase Wine upstream del 12 de septiembre de 2026

**Gate W1 aprobada dentro de BlesKernOS con una y dos vCPU:** el binario real
Wine 11.0 carga ntdll.so, imprime `wine-11.0` y sale con estado 0. No se parchearon
Wine, glibc ni ld.so. PT_INTERP/RUNPATH se ajustan durante el empaquetado y se
registran sus hashes originales/finales.

**W2 pendiente:** wineserver real llega a crear sus directorios y aborta:
`/tmp/.wine-0 must not be accessible by other users`. La reproduccion minima
muestra `mkdir(0700)` seguido de `stat` con modo `0755`. Falta implementar
metadatos/permisos POSIX persistentes y despues comprobar clientes Wine reales.
No estan aprobados wineboot, prefix persistente, X11, Notepad ni Office 95.

| Validacion | Evidencia en ports/wine/build | Resultado |
|---|---|---|
| 30 glibc + ABI + 3 ELF heredados, 1 CPU, mas W1 | wine-final-single/result.json | PASS |
| 30 glibc + ABI + 3 ELF heredados, 2 CPU | wine-phase-smp-retest/result.json | PASS |
| Fork/exec, shared/locks y W1 con kernel final, 2 CPU | wine-final-smp/result.json | PASS |
| Cinco consumidores OLE/COM/storage heredados | wine-phase-legacy/serial.log | PASS |
| Boot final 4 MiB, kernel mayor que el limite anterior | wine-final-boot4m/result.json | PASS |
| Layout, BSS, heap y reserva de instaladores | wine-final-layout.log | PASS |
| Rechazos por exceso de archivo/BSS/sectores | layout-negative.json | PASS |
| Primer Wine real antes de corregir readlink | w1-01/serial.log | Fallo reproducido |
| realpath minimo antes de corregir readlink | realpath-before/result.json | Fallo reproducido |
| realpath y W1 despues de corregirlo | w1-02/result.json | PASS |
| Wineserver real foreground | w2-01/serial.log | Bloqueo por permisos |
| Reproductor de modos POSIX | w2-permissions-repro/serial.log | 0700 solicitado, 0755 observado |

Casos nuevos: 28 verifica MAP_SHARED anonimo/archivo, aliases con offset,
read/write coherentes, msync, descriptor de solo lectura, close/unlink, fork y
futex entre procesos. 29 verifica locks POSIX, conflicto, GETLK, split,
SETLKW, close/exit y rangos mayores de 4 GiB. 30 verifica readlink/readlinkat
sobre nodos normales y realpath de glibc. Son contratos acotados; ver limitaciones.

La primera bateria SMP (`wine-phase-smp`) fallo en dos lecturas (casos 25/26)
y registra timeouts ATA, incluso durante boot antes de los casos nuevos. Habia
varios QEMU y un build concurrentes. La repeticion sin otros QEMU paso completa;
se conserva el fallo original. La dependencia respecto a carga del host es una
hipotesis sustentada por esos registros, no una correccion del driver ATA.

La reserva del kernel pasa de 1024 a 1280 KiB. El binario final ocupa
1.051.684 bytes; conserva asserts de imagen, BSS y solapamiento. La tabla grande
de ventanas Win32 se asigna al usarla. Ver [KERNEL_LAYOUT.md](KERNEL_LAYOUT.md).
No se modificaron las imagenes base ni discos del usuario: cada prueba migra una
copia FAT32 privada a la nueva reserva.

Wine 11.0 se construyo e instalo en DESTDIR en WSL con X11, FreeType y Fontconfig.
Fuentes fijadas en `versions/wine-11.0.json`; archivo fuente conservado en
`build/reference/wine-11.0.tar.gz`. Se corrigio el toolchain de Winebuild para
no producir import libraries ELF64; sin parches de codigo upstream.
El paquete bootstrap y sus hashes estan en `build/upstream-bootstrap`;
`package/wine-11.0-i386` contiene la instalacion completa y dependencias.
La instalacion completa NO tiene una gate funcional dentro del guest: W1 usa
el bootstrap identificado. OpenGL/Direct3D y sonido no fueron habilitados por
este configure. El build WSL no se usa como prueba de compatibilidad del guest.

La siguiente tarea concreta es corregir el contrato de permisos del filesystem,
repetir `permissions-gap.c`, volver a ejecutar wineserver y probar un cliente
Wine real. Despues vendran SO_PASSCRED/fchdir u otras llamadas que aparezcan,
wineboot/persistencia y el bridge X11. No se modifica la comprobacion de Wine
para ocultar los permisos ni se declara W2 aprobada porque el proceso arranque.

## Base anterior cerrada: 27 casos

Serie del 12 de septiembre de 2026. **27/27 casos glibc aprobados con uno y dos vCPU,
mas la prueba ABI y tres pruebas ELF heredadas. Wine y Office 95 siguen sin
 ejecutarse por esta ruta.** Ambas ejecuciones terminaron sin fallos fatales de hilos.

Entorno: WSL Ubuntu 24.04, GCC 13 multilib, glibc 2.39 i386 original; QEMU TCG,
Pentium III y 256 MiB para runtime/Win32. PT_INTERP y RPATH apuntan a
`/WINEGATE/lib`. Los manifiestos guardan hashes y comandos; no se usa la libc
 didactica de BlesK para estos casos. No se parchearon libc ni ld.so upstream.

| Casos | Comportamiento ejercitado | Resultado con uno y dos vCPU |
|---|---|---|
| 1–3 | Inicio dinamico, auxv y malloc/free | PASS |
| 4–6 | pthread create/join, TLS por hilo, mutex/futex | PASS |
| 7 | mmap/mprotect y MAP_FIXED dentro de reserva | PASS |
| 8 | fork/exec/wait con glibc | PASS |
| 9–12 | pipe, poll/select, socketpair, SCM_RIGHTS | PASS |
| 13–14 | Handler, altstack y SIGSEGV/ucontext | PASS |
| 15–18 | Directorios, dlopen/dlsym, archivos y reloj | PASS |
| 19 | Mascaras, pendientes, anidamiento, sigreturn y x87/XMM | PASS |
| 20 | Futex real, timeout, WAKE, clear_child_tid y EINTR | PASS |
| 21 | Fork anidado, memoria/TLS privados, FD compartido y wait/status | PASS |
| 22 | Exec: PID, argv/envp, CLOEXEC, acciones y mascara | PASS |
| 23 | Backpressure, PIPE_BUF, NONBLOCK y ciclos SCM_RIGHTS | PASS |
| 24 | Unix bind/listen/accept4, SO_PEERCRED, nombres pathname/abstractos | PASS |
| 25 | Dirfd relativo, *at, cookies, archivos vacios y rename | PASS |
| 26 | Mutex robusto: propietario muerto, EOWNERDEAD y recuperacion | PASS |
| 27 | DSO: referencias, dladdr, TLS por hilo y doce recargas | PASS |

La prueba ABI sin libc conserva los seis registros de argumentos y comprueba
 errores y old_mmap. LINUXPIE, INTERPPIE y GLIBCHEL pasan con el interprete
 didactico reconstruido. El harness rechaza fallos fatales de cualquier hilo,
 aunque main devuelva cero, y contabiliza ejecuciones repetidas.

La identidad constante de stat reproducia el fallo de `gate_value` buscado en
libc. Tras corregir VFS, dlopen/dlsym pasan sin modificar ld.so. La identidad FAT
 parte del volumen y registro corto: reconoce alias 8.3, distingue archivos
 vacios y se conserva al escribir y renombrar dentro del directorio.

La bateria expuso una carrera de entrada a int 0x80: una IRQ podia normalizar
GS antes de guardarlo. La interrupt gate protege la captura y restaura IF antes
 del dispatcher. En SMP, otro caso encontro un zombie ya consumido por waitpid
 pero aun visible durante su liberacion; deja de ser esperable al consumir el
 estado bajo el lock de tareas.

Regresiones: cinco consumidores heredados OLEAUT.EXE, COMTEST.EXE, STORAGE.EXE
 y dos POSIX aprobados. El arranque normal con 4 MiB alcanza `GUI_RUN entered`.
Esto no prueba Office real ni todas las aplicaciones con 4 MiB. Persisten las
 advertencias de drivers ausentes y falta de memoria para AC97 en ese perfil.

Evidencia local:

- `ports/wine/build/full03/`: 27 casos, ABI y tres ELF heredados aprobados;
  disco aislado, kernel, hashes, manifiesto, serial.log y result.json.
- `ports/wine/build/final-smp/`: 27 casos, ABI y tres ELF heredados aprobados con
  dos vCPU, incluyendo la correccion final de waitpid; `fatal_faults` vacio.
- `ports/wine/build/final-legacy/`: cinco consumidores OLE/COM/storage aprobados
  con la compilacion final; conserva tambien los documentos CFB extraidos.
- `ports/wine/build/final-boot4m/`: arranque de 4 MiB aprobado con la compilacion final.
- `ports/wine/build/host-runtime.json`: control positivo en Linux, separado del guest.
- `ports/wine/build/reference/wine-configure.log`: configure Wine 11.0 detenido
  por falta de desarrollo X11 i386; no es una ejecucion de Wine.

Kernel: 1.047.588 bytes, bajo el limite de 1.048.576. Se conservan los asserts
 de imagen y BSS. Build con -Oz y eliminacion de secciones no referenciadas;
 desktop/widget usan -Oz, con las primitivas graficas restantes en -O2.
Permanecen advertencias GNU-stack, funciones no usadas y desfase de timestamps
WSL/NTFS. Los fallos exploratorios se conservan en directorios anteriores y no
 deben confundirse con la evidencia final.

Ver [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md): MAP_SHARED, locking y otras
 primitivas necesarias para wineserver siguen pendientes.

