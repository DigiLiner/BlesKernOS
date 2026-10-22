# Auditoría previa a la implementación LBS (2026-10-18)

> Auditoría histórica. Estado y evidencias actuales: [LBS-TOTAL.md](LBS-TOTAL.md).

Se inspeccionaron las rutas siguientes antes de modificar el cargador.

* `kernel/exec/elf_loader.c`: ET_REL nativo con importaciones transformadas en
  trampolines de syscalls, y ET_EXEC/ET_DYN Linux con PT_INTERP, DT_NEEDED,
  REL, COPY y TLS. El intérprete Linux se ejecuta en Ring 3. No se necesita
  otro cargador de kernel para LBS.
* `sdk/lib/posix_dlfcn.c`: cargador Ring 3 ET_DYN con SysV hash, REL i386,
  dependencias, arrays init/fini y referencias. Límites originales: ocho módulos,
  símbolos globales indiscriminados, ciclos aceptados, handles desreferenciados
  antes de validar y tablas parcialmente verificadas. No implementa TLS tardío.
* `sdk/README.md`, Makefile: SDK estático nativo, POSIX, Linux, SDL2, TinyGL,
  Mesa y libm; las SO de Linux/Wine tienen su propia ruta de integración.
* `sdk/include/bleskernos*.h`, `sdk/lib/syscalls.c`, `kernel/api/syscall.c`:
  ABI nativa 17 y API pública 40. Las funciones públicas bk_* de ET_REL pasan
  por el despachador del kernel; nunca se deben convertir en llamadas directas
  a direcciones supervisor. POSIX añade sockets TCP/UDP, polling e hilos.
* `gui/{widget,windows,event,desktop}.c`, `kernel/api/vision_{ipc,resources}.c`,
  `system/services/vision.c`: ventanas y widgets opacos, eventos y callbacks
  de usuario, recursos con propietario. Se preserva la estética y el compositor.
* `gui/{gfx,raster,image,font,real_font,compositor,gpu_compositor}.c`: 2D,
  clipping, imágenes, escalado, composición y caché de tipografía existentes.
  `bk_gui_window_present_xrgb` permite presentación de buffers de usuario.
* `libs/src`, `sdk/lib/tinygl.c`: TinyGL con un contexto actual por proceso;
  `libs/Mesa-3.5`, `sdk/lib/mesa.c`: Mesa/OSMesa y backend GFX3D con fallback,
  texturas, buffers de profundidad y presentación a Vision. GFX3D tiene drivers
  VirtIO/VirGL, SVGA y otros; disponibilidad depende del dispositivo y estado.
* `kernel/drivers/audio`, `kernel/audio`, `programs/midamp.c`: PCM u8 mono,
  tono, drivers y reproducción existente. El SDK no publica un mezclador
  multicanal ni enumera todas las tasas por dispositivo.
* `kernel/drivers/network`, `kernel/net`, `sdk/lib/posix_net.c`: TCP/UDP,
  DNS, servidores, timeouts y TLS BearSSL. TLS usa certificados y reloj;
  su fallback de entropía temporal NO acredita aleatoriedad criptográfica.
* `programs/viewer/lib`: BKV, AVI DIB/MJPEG/PCM, GIF, MPEG1/MP2 reutilizables.
  `gui/image.c`, `jpeg.c`, `svg.c`: imágenes BMP/PNG/GIF/JPEG/SVG públicas.
* `sdk/include/bleskernos_input.h`, eventos Vision y SDL2: entrada con foco,
  preferencias y cursores. No se encontró API nativa pública de gamepads.
* `sdk/lib/print.c`, `system/services/printspool.c`, `system/printers`:
  trabajos BPJ1, spool y puertos; cancelación antes de submit. No confundir
  archivo generado con éxito de impresión física.
* `libs/bearssl`: criptografía existente. Se puede compilar SHA256 en Ring 3
  sin copiar ni inventar algoritmos. No usar getrandom temporal como CSPRNG.
* `kernel/mm/{paging,pager,usercopy,memory}.c`, `kernel/api/native_memory.c`:
  CR3 por proceso, validación/copia/pinning de buffers, recursos por propietario,
  RO/RW en PTE; i386 sin NX. UID/GID POSIX sintéticos no son autorización.
  No hay una API de capacidades generales aplicada por el kernel.
* Aplicaciones consumidoras: Editor, Filebrowser, Paint, Imageviewer, Midamp,
  Viewer, controles, escritorio, salvapantallas, Wine y SDL. Hay cambios locales
  previos en varias de ellas; no deben sobrescribirse.

## Decisiones

Ampliar dlopen existente. LBS = ELF32 ET_DYN con SONAME, SysV hash y ABI
mayor explícita. Cada módulo exporta sólo su API. Puente Ring 3 optativo para
las importaciones del SDK: resuelve exclusivamente trampolines autorizados del
ET_REL, sin búsqueda de símbolos privados ni nuevas syscalls. Los módulos
con algoritmos existentes se recompilan PIC. No mezclar libc Linux y nativa.
El ejecutable de demostración hace su carga inicial antes de crear recursos.

El código de módulos inicialmente ocupa memoria privada en cada proceso:
referencias dentro de un proceso NO comparten páginas físicas entre procesos.
Compartir código exigiría mappings de archivo, relocaciones sin textrels y
GOT/datos privados; no debe improvisarse un caché global con alias escribibles.
