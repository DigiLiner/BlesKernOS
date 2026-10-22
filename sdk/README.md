# SDK nativo de BlesKernOS

Este SDK genera aplicaciones ELF32 relocatables que se comunican con el
kernel exclusivamente mediante `int 0x80`. Una aplicación sólo incluye
`sdk/include/bleskernos.h`; no debe incluir archivos de `kernel/`, `gui/` ni
drivers.

El punto de entrada actual es:

```c
void bleskernos_program_main(void *unused);
```

Compilación manual:

```sh
gcc -m32 -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic \
  -fno-pie -fno-stack-protector -Isdk/include -c app.c -o app.raw.o
ld -m elf_i386 -r app.raw.o build/sdk/libblesk.a -o app.o
```

`app.o` puede copiarse a `/SYSTEM/PROGRAMS`. La syscall ABI 17 de x86 ofrece consola,
archivos, directorios, memoria, tiempo, administración de procesos, resolución
DNS y sockets TCP cliente. Las llamadas nuevas son `bk_dns_resolve`,
`bk_socket`, `bk_connect`, `bk_send`, `bk_recv`, `bk_socket_close` y el cliente
de alto nivel `bk_http_get` (acepta `http://` y `https://`); cada proceso
dispone de hasta cuatro sockets.

## Capa POSIX para aplicaciones x86

`libblesk_posix.a` proporciona una capa de compatibilidad de userland sobre la
ABI Ring 3. Publica headers independientes del kernel para `unistd.h`,
`fcntl.h`, `errno.h`, `dirent.h`, `stdio.h`, `stdlib.h`, `string.h`, `strings.h`,
`ctype.h`, `time.h`, `signal.h`, `sched.h`, `pthread.h`, `poll.h`, `netdb.h`,
`arpa/inet.h`, `netinet/in.h`, `sys/types.h`, `sys/stat.h`, `sys/wait.h`,
`sys/mman.h`, `sys/socket.h`, `sys/select.h`, `sys/time.h`, `sys/utsname.h`,
`semaphore.h`, `getopt.h`, `locale.h`, `termios.h`, `libgen.h`, `dlfcn.h`,
`math.h` y `float.h`.

Actualmente incluye descriptores `open/read/write/close`, `dup/dup2`, redireccion
de stdio, seek y truncado,
sincronización, `stat/fstat`, directorios, directorio de trabajo, creación y
eliminación de archivos/directorios, `errno`, reloj monotónico, sleeps,
memoria dinámica, cadenas, stdio básico, variables de entorno, ordenación,
conversión numérica, PID, wait/kill y yield. También incluye `mmap` anónimo y
de archivos privados, pthreads joinable/detached, mutexes, variables de
condición, rwlocks, TLS, sockets IPv4 TCP/UDP cliente y servidor, DNS,
`getaddrinfo`, modo no bloqueante, pipes, `poll`, `select` y semáforos. La capa
de portabilidad agrega `getopt_long`, locale C/UTF-8, calendario, `uname`,
`sysconf`, terminal básica, `ioctl(TIOCGWINSZ)`, rutas canónicas y `libm`.
Tambien acepta `main(argc, argv)` con comillas y escapes, `posix_spawn`,
`posix_spawnp`, senales locales cooperativas, mascaras, `raise`, `alarm`,
temporales, `sscanf`, UID/GID sinteticos, `chmod` y `umask`. Las rutas son las rutas VFS de
BlesKernOS; no se simula un host Unix por debajo.

Los procesos creados heredan descriptores VFS con la misma descripcion abierta
y `posix_spawn_file_actions` implementa `open`, `close` y `dup2`. La familia
`execv/execvp/execl` usa un reemplazo compatible spawn-wait-exit; conserva el
resultado observable por el padre, aunque el proceso interno recibe otro PID.
`system()` ejecuta el shell Ring 3.

La compatibilidad Xlib inicial publica `X11/Xlib.h`, ventanas, eventos de
teclado/mouse/expose, GC, colores y primitivas 2D sobre la GUI Ring 3. No usa
un servidor X externo: traduce Xlib directamente al compositor de BlesKernOS.
`dlopen(NULL)` y `dlsym` permiten descubrir simbolos del runtime estatico. El
loader inicial tambien carga modulos ELF32/i386 `ET_DYN` independientes,
procesa `PT_LOAD`, `DT_HASH`, tablas `REL`, constructores/destructores y las
relocaciones `R_386_32`, `PC32`, `GLOB_DAT`, `JMP_SLOT` y `RELATIVE`. Ahora
resuelve `DT_NEEDED` recursivamente junto al módulo y en
`/SYSTEM/LIBS/POSIX`, comparte bibliotecas mediante referencias y descarga
dependencias en orden inverso. Todavía faltan versionado GNU de símbolos y
ampliar el DTV/TLS cuando una biblioteca se carga tarde mediante `dlopen`.

Compilación:

```sh
gcc -m32 -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic \
  -fno-pie -fno-stack-protector -Isdk/include -c app.c -o app.raw.o
ld -m elf_i386 -r -u bleskernos_program_main app.raw.o build/sdk/libblesk_posix.a \
  build/sdk/libblesk.a -o app.o
```

El orden de las bibliotecas es intencional. `sdk/examples/posix_smoke.c` prueba
I/O, seek, stat, directorios, tiempo, `mmap`, pthreads y limpieza de recursos. En una edición
developer se instala como `/SYSTEM/TESTS/NATIVE/POSIX_TEST.BEX`.

Como referencia de portabilidad, esta capa apunta aproximadamente al 35–45%
del software POSIX antiguo o liviano cuando se dispone del código fuente y se
permite recompilarlo estáticamente. No es una estimación de compatibilidad
binaria Linux completa: el loader de modulos no convierte por si solo un
ejecutable Linux ni reemplaza glibc.

## ABI Linux i386 inicial

La syscall ABI 17 agrega una personalidad por proceso. El adaptador entra en
ella una sola vez y, desde ese punto, `int 0x80` usa numeros y registros Linux
i386. La primera tabla traduce `exit/exit_group`, `read`, `write`, `open`,
`close`, `waitpid`, `unlink`, `chdir`, `lseek`, `getpid/getppid`, `kill`,
`rename`, `mkdir`, `dup/dup2`, `fsync`, `sched_yield` y `getcwd`; cualquier
numero no implementado devuelve `-ENOSYS`. La segunda etapa agrega `brk`,
`mmap`/`mmap2`, `munmap` y `mprotect`, con regiones alineadas y contabilizadas
por proceso, mappings anónimos o respaldados por archivos y liberación al
terminar el proceso. ABI 17 suma `set_thread_area/get_thread_area`, TLS i386 y
la base de `futex` privado. `mprotect` aplica realmente RW frente a RO/RX en
las PTE y hace shootdown TLB SMP; i486 no dispone del bit NX para impedir
ejecución en páginas de datos.
También traduce `fcntl64`, `getuid32/getgid32/geteuid32/getegid32`,
`getrandom`, `prlimit64`, `sched_getaffinity/sched_setaffinity`,
`clock_nanosleep` y `tkill/tgkill`.
También inicia `execve`: carga la nueva imagen y hereda sus descriptores. La
implementación continúa siendo spawn-replace y no conserva todavía el PID, por
lo que aún no se anuncia como semántica Linux completa.
Para runtimes reales también implementa `uname`, `gettimeofday`,
`clock_gettime/clock_getres`, `stat64/lstat64/fstat64`, `access`, UID/GID,
`gettid`, `set_tid_address`, `set_robust_list` e `ioctl(TIOCGWINSZ)`.
También expone `time`, `nanosleep`, `sysinfo`, `getrlimit/ugetrlimit` y
`madvise`, usadas por SDL y motores de juegos antiguos durante el arranque.

`libblesk_linux.a` aporta el arranque con `main(argc, argv)`, `errno`, wrappers
de esas syscalls, cadenas y un allocator provisional aislado. Se enlaza asi:

```sh
ld -m elf_i386 -r -u bleskernos_program_main app.raw.o \
  build/sdk/libblesk_linux.a build/sdk/libblesk.a -o app.o
```

Esto permite empezar a recompilar codigo que espera la ABI Linux sin mezclarla
con la ABI nativa. El cargador del kernel ya reconoce `ET_EXEC`, `ET_DYN` y
`PT_INTERP`, y ejecuta PIE ELF32/i386 tanto directos como mediante un
intérprete Ring 3. El kernel construye el stack Linux inicial con `argc`, `argv`,
`envp` y `auxv` (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`).
`ld-blesk.so` recibe `AT_BASE` y `AT_ENTRY`, y está instalado también como
`/lib/ld-linux.so.2`. El loader carga hasta 24 objetos `DT_NEEDED` de forma
recursiva, buscando junto al ejecutable, en `/lib`, `/usr/lib`,
`/SYSTEM/LIBS/POSIX` y `/SYSTEM/LIBS`. Resuelve símbolos globales con tablas
SysV o GNU hash, y aplica `R_386_RELATIVE`, `R_386_32`, `R_386_PC32`,
`R_386_COPY`, `R_386_GLOB_DAT` y `R_386_JMP_SLOT`. `DT_INIT` y
`DT_INIT_ARRAY` se ejecutan en Ring 3 antes del entrypoint. La prueba
`DYNAMIC.ELF` valida `PT_INTERP`, dependencia externa, PLT, constructor,
`PT_TLS` con `%gs`, `R_386_TLS_TPOFF`, `R_386_TLS_DTPMOD32`,
`R_386_TLS_DTPOFF32` y el resolver general-dynamic `__tls_get_addr` con DTV.
`DT_RPATH`/`DT_RUNPATH` aceptan listas
separadas por `:` y `$ORIGIN`/`${ORIGIN}` para bibliotecas privadas.
ABI 17 incorpora además `clone(2)` para hilos que comparten VM, archivos y
manejadores de señales. Soporta `CLONE_SETTLS`, `PARENT_SETTID`,
`CHILD_SETTID`, `CHILD_CLEARTID`, `set_tid_address` y retorno dual padre/hijo;
la prueba dinámica verifica la creación y terminación del hilo. Las variantes
tipo `fork`, que crean un proceso separado, todavía se rechazan: ya existe CR3
por proceso, pero falta clonar el espacio con copy-on-write.

Los `ET_EXEC` se cargan ahora en su dirección original. La ventana Linux
tradicional admite un entrypoint en `0x08048000`; sus `PT_LOAD` usan tablas
privadas y permisos RW/RO-RX por proceso. `EXECFIX.ELF` ejecuta dos instancias
simultáneas con las mismas direcciones virtuales y comprueba su aislamiento.

La edición developer instala además una capa binaria inicial como
`/lib/libc.so.6`. Exporta `__libc_start_main`, `read`, `write`, `exit`, `_exit`,
`strlen` y `puts`, suficiente para que `GLIBCHEL.ELF`, un ejecutable i386
dinámico generado por GCC sin modificar, atraviese `/lib/ld-linux.so.2`,
resuelva `DT_NEEDED` y ejecute `main(argc, argv)`. Es deliberadamente un shim
de arranque, no GNU glibc completa ni una promesa de compatibilidad con todos
sus símbolos o versiones.

No significa aún que un binario de una distribución arranque directamente:
faltan `fork`, colas futex con espera dirigida, señales Linux completas, VDSO,
TLS por hilo para `dlopen` tardío y una libc binariamente amplia. Los headers
`elf.h`, `link.h`, `features.h` y `sys/cdefs.h` forman la base de esa siguiente
etapa. `/SYSTEM/TESTS/NATIVE/LINUX_ABI_TEST.BEX` valida la personalidad y la
memoria virtual, `LINUXPIE.ELF` valida un ejecutable PIE real y `DLMOD.SO`
valida el loader dinámico de módulos en la edición developer.

No se anuncian todavía `fork` con copia de espacio de direcciones, una terminal interactiva completa, enlaces,
permisos/propietarios Unix, IPv6 ni señales asíncronas. Los pipes son locales
al proceso y sus hilos. `MAP_SHARED`/`msync`
siguen pendientes. Las senales locales se entregan cooperativamente y no
como interrupciones del kernel para `raise`; las enviadas con `kill` si usan
upcalls asincronos del scheduler. `SIGKILL` sigue siendo no interceptable y
`SIGSTOP` aun no esta implementada. Los timestamps y permisos de `stat` son sintéticos porque FAT no
almacena toda la metadata POSIX.

## SDL2 para programas gráficos

`libblesk_sdl2.a` es una capa SDL2 Ring 3 sobre los callbacks/upcalls de la GUI
pública. Incluye ventanas, superficies ARGB8888, carga BMP, cola de teclado y
mouse, timers, blit normal y escalado, renderer 2D por software, primitivas,
texturas estáticas/streaming y audio U8/S16 mono o estéreo sobre PCM. También
publica las entradas SDL 1.2 más usadas (`SDL_SetVideoMode`, `SDL_Flip`, caption
y video surface). Esto permite portar programas SDL conservando
su `main()` y su bucle de eventos; no integra estructuras GUI privadas en el
programa.

`<SDL2/SDL_blesk.h>` ofrece `SDL_BleskGetWindow(SDL_Window *)`: devuelve la
ventana nativa opaca prestada, válida hasta `SDL_DestroyWindow()`. Permite
publicar con `bk_gui_window_present_xrgb` sin copiar primero a una superficie
SDL. SDL conserva la propiedad y entrega los eventos. Presentar superficies
GPU sin descarga requiere además soporte real del compositor de ventanas.

```sh
gcc -m32 -ffreestanding -fno-builtin -nostdlib -nostdinc -fno-pic \
  -fno-pie -fno-stack-protector -Isdk/include -c game.c -o game.raw.o
ld -m elf_i386 -r -u bleskernos_program_main game.raw.o \
  build/sdk/libblesk_sdl2.a build/sdk/libblesk_posix.a build/sdk/libm.a \
  build/sdk/libblesk.a -o GAME.BEX
```

El orden y `-u bleskernos_program_main` son necesarios para extraer del archivo
SDL el adaptador de `main()`. `sdk/examples/sdl_port_demo.c` y
`/SYSTEM/TESTS/NATIVE/SDL_DEMO.BEX` verifican renderer, textura, presentación y
eventos. Por ahora no se promete compatibilidad SDL completa: joysticks,
OpenGL-context, mezcla avanzada y formatos de píxel distintos de ARGB8888 requieren
backends adicionales. Para 3D nativo se mantienen TinyGL y Mesa/OSMesa.

## API de aplicaciones nativas

Los programas y comandos ET_REL que necesitan servicios de escritorio o de
administración incluyen `sdk/include/bleskernos_api.h`. Esta API v23 también se
ejecuta en Ring 3 y el cargador transforma sus símbolos en transiciones
controladas al kernel.

La API entrega estructuras públicas por copia (`bk_block_info_t`,
`bk_partition_info_t`, `bk_volume_info_t`, `bk_volume_check_report_t`,
`bk_pci_info_t` y `bk_process_info_t`). Una aplicación no
debe incluir `kernel/include/api.h` ni recibir `block_device_t`, `fat_fs_t`,
`pci_device_t`, estructuras de drivers o punteros internos equivalentes.

`bk_device_check_volume()` ejecuta un análisis FAT de sólo lectura. El informe
público permite consultar copias FAT divergentes, clusters perdidos, cruzados,
malos o reservados, cadenas circulares o inválidas y tamaños incoherentes sin
exponer al programa las estructuras internas del controlador. El mismo informe
incluye archivos fragmentados, cantidad de fragmentos y la mayor extensión
libre. `bk_device_partition_count()` y `bk_device_partition_info()` permiten
consultar las cuatro entradas primarias de cada MBR sin dar acceso crudo al
dispositivo.

`bk_device_repair_volume()` aplica la reparación conservadora usada por
`checkdisk /fix` y ScanDisk. Antes de escribir exige que el árbol completo de
directorios sea legible; sincroniza las FAT, repara la copia de arranque FAT32,
corta cadenas dañadas y libera clusters demostrablemente huérfanos. Devuelve un
informe separado con cada cambio y vuelve a analizar el volumen al terminar.

La misma cabecera declara una GUI Ring 3 de objetos opacos. Las aplicaciones
pueden crear ventanas y dibujar superficies mediante `bk_gui_*` sin incluir
`gui/gui.h` ni conocer la representación interna de ventanas y escritorios.

La API v22 agrega el catálogo compartido `/SYSTEM/GRAPHICS.PAK`.
`bk_graphics_icon_load("Folder", &image)` carga un recurso por nombre,
`bk_graphics_icon_count()` y `bk_graphics_icon_name()` permiten enumerarlo, y
`bk_gui_image_free()` libera la imagen decodificada. El archivo
`assets/graphics/GRAPHICS.CSV` contiene el catálogo disponible en la imagen
oficial.

La API v23 agrega `bk_gui_widget_set_icon(button, "FolderOpen")`: el botón
estándar conserva el recurso y lo libera automáticamente al reemplazarlo o
destruirse. Esto sirve para acciones como abrir, copiar, pegar, guardar,
actualizar y eliminar sin duplicar imágenes dentro del programa.

La API v24 agrega cursores gráficos temporales por proceso. Una aplicación puede
usar `bk_gui_cursor_set_resource("Brush", 5, 27)` para activar un cursor de
`GRAPHICS.PAK` y `bk_gui_cursor_reset()` para volver a la flecha. El escritorio
restaura automáticamente el cursor al cerrar el proceso que lo solicitó. Los
recursos admitidos miden hasta 32x32 píxeles y conservan transparencia alfa.

La API v30 agrega avisos modales compartidos. `bk_gui_error()` muestra un fallo
normal, `bk_gui_network_error()` aplica la presentacion de errores de red y
`bk_gui_alert()` permite elegir `BK_ALERT_INFO`, `BK_ALERT_WARNING`,
`BK_ALERT_ERROR` o `BK_ALERT_NETWORK`, junto con un codigo de diagnostico.

La API v39 agrega `bk_net_http_exchange_quiet()` con la misma firma y códigos
que `bk_net_http_exchange()`. No abre avisos de red: el llamador decide cómo
presentar los errores, por ejemplo desde un worker de recursos de navegador.
La función anterior conserva sus avisos modales.

Ejemplo de enumeración de discos:

```c
#include <bleskernos_api.h>

void bleskernos_program_main(void *unused) {
    bk_block_info_t disk;
    (void)unused;
    for (uint32_t i = 0; i < bk_device_block_count(); i++) {
        if (bk_device_block_info(i, &disk))
            bk_console_printf("%s: %u sectores\n", disk.name,
                              disk.sector_count);
    }
    bk_proc_exit();
}
```

## TinyGL

El SDK publica TinyGL sin depender de headers privados del kernel:

- `include/TGL/gl.h`: API OpenGL-compatible de TinyGL.
- `include/tinygl/zbuffer.h`: API de framebuffer de bajo nivel.
- `include/bleskernos_tinygl.h`: contexto y presentación adaptados a BlesKernOS.
- `libblesk_tinygl.a`: TinyGL más la capa de integración del SDK.
- `examples/tinygl_triangle.c`: ejemplo mínimo.

Compilación típica:

```sh
gcc -m32 -ffreestanding -fno-builtin -nostdlib -nostdinc -Os \
  -fno-pic -fno-pie -fno-stack-protector -Isdk/include \
  -c app.c -o app.raw.o
ld -m elf_i386 -r app.raw.o build/sdk/libblesk_tinygl.a \
  build/sdk/libblesk.a -o app.o
```

`bk_tinygl_create()` crea el ZBuffer y el buffer lineal; después de dibujar,
`bk_tinygl_present()` actualiza los píxeles que la aplicación puede copiar a
su superficie GUI.


## Mesa 3.5 / GFX3D

Mesa 3.5 se compila como una segunda implementación OpenGL, independiente de
TinyGL. El backend de BlesKernOS está insertado en OSMesa/TNL: Mesa conserva
estado, matrices, iluminación y clipping, mientras GFX3D rasteriza el
subconjunto del viewport de 3D Plus.

Archivos publicados por el SDK:

- `include/GL/gl.h`, `include/GL/glext.h` y `include/GL/osmesa.h`.
- `include/bleskernos_mesa.h`: contextos, renderer estricto, presentación directa y estadísticas.
- `libblesk_mesa.a`: Mesa 3.5, OSMesa y el backend GFX3D.
- `examples/mesa_triangle.c`: prueba mínima AUTO.
- `examples/mesa_gpu_strict_test.c`: stress GPU-only de 10.000 frames.

Compilación típica:

```sh
gcc -m32 -mfpmath=387 -mno-sse -mno-sse2 -ffreestanding -fno-builtin \
  -nostdlib -nostdinc -Os -fno-pic -fno-pie -fno-stack-protector \
  -Isdk/include -c app.c -o app.raw.o
ld -m elf_i386 -r app.raw.o build/sdk/libblesk_mesa.a \
  build/sdk/libblesk.a -o app.o
```

`BK_MESA_RENDERER_GPU_STRICT` convierte cualquier fallback inesperado en un
error consultable con `bk_mesa_strict_failed()` y
`bk_mesa_last_fallback_string()`. `bk_mesa_present_gpu()` conserva el resultado
en VRAM y `bk_mesa_attach_to_window()` lo conecta al compositor sin readback.
La ABI GFX3D v3 sincroniza color y Z16 cuando los modos AUTO/GPU alternan con
SWRAST.

Para probar aceleración real hay que comprobar que `draw_calls` y `triangles`
aumenten y que `fallbacks`, `strict_failures` y `downloads` permanezcan en cero.
La tabla completa de estados acelerados y limitaciones está en
`docs/graphics/mesa35.md`.

Mesa y TinyGL exportan los mismos símbolos OpenGL (`glBegin`, `glClear`, etc.).
Por eso una aplicación debe enlazar **una sola** de las dos bibliotecas. El
sistema puede distribuir ambas sin conflicto porque cada programa ET_REL lleva
la implementación elegida dentro de su propio objeto enlazado.


La API v40 añade mantenimiento de archivos (`bk_file_touch`, modos POSIX,
espacio por ruta y sincronización), entrada nativa `bk_console_read`, montaje
y desmontaje de volúmenes secundarios, edición acotada de MBR primarios,
información de dispositivos USB y snapshots de sockets nativos.
`bk_console_read` espera la entrada de la terminal sin retener un dominio
grueso del kernel. Los nuevos DTO de sockets están en
`include/bleskernos_socket_info.h`. Ver `docs/terminal.md` para los comandos,
los formatos admitidos y las pruebas.

## Bibliotecas dinámicas nativas LBS

`make lbs` construye las doce bibliotecas nativas y el backend TinyGL opcional
como ELF32/i386 ET_DYN. Se instalan en `/SYSTEM/`; el cargador
`dlopen` del SDK conserva las SO y añade dependencias, ABI, controles de
recursos, serialización y diagnóstico para las LBS. El cargador Linux/Wine
permanece independiente.

Ver [guía LBS](../docs/native/LBS.md), [auditoría](../docs/native/LBS-AUDIT.md),
[catálogo de exportaciones](../docs/native/LBS-EXPORTS.md) y
[resultados](../docs/native/LBS-REPORT.md). Los ejemplos están en
`examples/lbs_demo.c` y `examples/lbs_startup_demo.c`.

La migración de aplicaciones/CPL y el enlace dinámico automático se describen en
[`docs/native/LBS-MIGRATION.md`](../docs/native/LBS-MIGRATION.md). Las LBS se instalan directamente en `/SYSTEM`.

Las LBS PIC cargadas con el SDK actual comparten páginas físicas de código y
constantes entre procesos, con datos/GOT privados. Requiere el kernel actualizado
y volver a enlazar clientes que incorporaban el cargador anterior.
[Mecanismo, pruebas y límites](../docs/native/LBS-SHARING.md).

### Runtime LBS y descriptores

Para clientes LBS usar `libblesk_lbs_posix.a` y `libblesk_lbs_native.a`, además
del puente/CRT requerido. Sus helpers de FD POSIX están aislados de los símbolos
nativos homónimos `bk_file_size` y `bk_file_write`. Las aplicaciones migradas y
las plantillas Codex con `USE_LBS=1` seleccionan estas variantes automáticamente.
Headers añadidos: `bleskernos_archive_lbs.h`, `bleskernos_hyperzip_lbs.h`,
`bleskernos_inflate_lbs.h`, `bleskernos_command_lbs.h`, `bleskernos_widgets_lbs.h`
y `bleskernos_raster_lbs.h`. Ver docs/native/LBS-MIGRATION-PLAN.md.
