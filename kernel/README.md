# Kernel de BlesKernOS 0.9

| Directorio | Responsabilidad |
| --- | --- |
| `core/` | Inicio del kernel, panic y perfiles de compatibilidad de hardware |
| `arch/x86/` | Entrada, descriptores, interrupciones, SMP, Ring 3 y linker scripts |
| `mm/` | Memoria, paginación y copia entre kernel y usuario |
| `sched/` | Tareas, scheduler, locks, dominios y contadores de rendimiento |
| `api/` | API nativa, exportaciones públicas y despacho de syscalls |
| `exec/` | Cargadores ELF/BEX y módulos residentes |
| `linux/` | Implementación de la ABI Linux i386 utilizada por Wine |
| `drivers/` | Drivers organizados por clase de dispositivo |
| `console/` | Consola VGA y fuentes auxiliares de consola |
| `ui/` | Diálogos y recursos del escritorio, idioma y splash |
| `audio/` | Sonidos de inicio, sistema y archivos de audio |
| `net/` | Servicios de red del kernel |
| `setup/` | Instalación, configuración inicial y recuperación |
| `libc/` | Runtime C freestanding y headers estándar en `libc/include/` |
| `include/` | Interfaces internas entre subsistemas |

El kernel ya no contiene el cargador PE/NE ni la implementación Win32/Win16
propia. Windows se ejecuta mediante Wine 11 sobre la ABI Linux i386.
Wine, wineserver y sus procesos auxiliares se inician al abrir una aplicación
Windows; no se precargan al iniciar el escritorio.
Se conservan las reservas de memoria que Wine necesita para imágenes de base
fija y los slots de syscalls retiradas, que devuelven `ENOSYS` sin renumerar
la ABI nativa. Un byte reservado mantiene también el layout de tareas para
los módulos nativos existentes.

Las builds normales siguen entrando por el Makefile de la raíz. Para código
freestanding, los headers estándar se buscan en `kernel/libc/include/`.
El estado de las herramientas Windows del self-hosting se documenta en
[la guía de self-hosting](../docs/build/self-hosting.md).

Las optimizaciones para arranque USB UHCI, detección del panel ATI Mobility M3
y actualización del administrador de procesos se documentan en
[rendimiento del C600](../docs/platform/c600-performance.md).

La API nativa 37 agrega deslizadores y casillas/opciones exclusivas al sistema
común de widgets. El SDK también expone el renderer de pestañas con esquinas
suaves y orden de capas compartido por los paneles y las herramientas del OS.
Los detalles están en [la guía del tema](../docs/platform/gui-theme.md).

El catálogo compartido `/SYSTEM/GRAPHICS.PAK` usa los recursos históricos de
KDE 1: 870 bitmaps con sus variantes y 71 alias compatibles con las
aplicaciones existentes. Se reconstruye sin red con `make graphics-pak`.
El catálogo, las fuentes y los avisos de procedencia se distribuyen con el
OS; la revisión de licencia individual de algunas imágenes sigue pendiente.
Consulte [la guía de recursos gráficos](../docs/graphics/GRAPHICS_PAK.md).

Los cursores globales usan Retrosmart blanco sin sombra, a 32×32 sin escalar.
Los 48 roles se distribuyen en `/SYSTEM/CURSORS`, con fuentes y licencia GPL-3.0.
Espera y trabajo en segundo plano conservan las animaciones originales; el
cargador de aplicaciones muestra trabajo mientras lee y prepara el programa.
Se reconstruyen sin red con `make cursors`. La prueba de animación con el mouse
quieto y 32 MiB se ejecuta con `python3 tests/kernel/run_cursor_smoke.py`.

Los caches de ventanas para poca RAM conservan un byte por pixel mediante una
paleta con los colores exactos del tema y grises neutros. VMware/VirtIO mantienen
su framebuffer de 32 bits; las opciones de 16, 256 y 65 mil colores del panel se
aplican por conversion de las regiones modificadas, sin otro framebuffer.
Vision recibe los cambios de fondo por un buzon de preferencias, y la estela
y el movimiento de ventanas se configuran sobre el escritorio real.

La prueba de estos ajustes con 32 MiB y VMware se ejecuta con
`python3 tests/kernel/run_ram_smoke.py --ram 32 --vga vmware --smoke --fixture preferences --accel kvm --cpu host --smp 2`.
