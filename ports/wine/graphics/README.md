# Puente gráfico Wine / Linux / GFX3D

Gallium también tiene una integración independiente de Wine y TinyX:
[runtime nativo, API y glmark2](../../../docs/graphics/NATIVE_GALLIUM.md).
Ambas rutas reutilizan el winsys común de `ports/gallium/virgl-winsys.c`.

El runtime incluye un backend GLX local (`libGL.so.1`) que selecciona Gallium
VirGL (`libbkvirgl.so.8`) cuando el driver ofrece el transporte programable.
En los demás drivers usa Mesa 3.5 (`libbkmesa.so.1`), que conserva el estado
OpenGL y envía rasterizado compatible al driver GFX3D disponible. El estado no
representable conserva el fallback de Mesa. TinyX recibe los píxeles por X11;
no se anuncia un servidor GLX inexistente. Wine acepta esta ruta únicamente
mediante el handshake explícito del patch 0004 de winex11.

La nueva ABI `/dev/blesgfx` valida dimensiones, geometría, memoria del usuario
y propiedad de superficies; libera recursos al cerrar el último descriptor.
La espera es una barrera para las entregas de ese descriptor, sin aceptar
fences externos del llamador.

**Alcance de Mesa 3.5:** OpenGL clásico; esa biblioteca no aporta GLSL.
Gallium VirGL aporta el renderizado programable necesario para WineD3D,
con las validaciones y límites descritos abajo. La opción del launcher `--software-gl` permite usar una instalación
opcional de `libOSMesa.so.8`, con sus dependencias; esas bibliotecas grandes no
se agregaron a la imagen normal. No se validó esa alternativa.

Compilación Mesa Linux:

```sh
python3 ports/wine/graphics/build-mesa.py
```

El Makefile recompila GLX y los backends cuando cambian sus fuentes. La imagen
ATA normal y `make user-update` dependen también de Gallium, sus dependencias
i386 y sus licencias; los instaladores derivados de ATA reciben ese runtime. El módulo Unix `winex11.so` incluido se recompiló con Wine 11.0,
patch 0004, OpenGL detectado y `--without-xxf86vm`. Reconstruir Wine requiere
que configure encuentre una biblioteca GL i386 y los headers X11/FreeType.

Arranque virtio con memoria apropiada para Wine:

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-ata-user.img --gl --cdrom media/installers/OFF97.iso
```

`--memory` permite cambiar los 1536 MiB predeterminados; `--smp`, los dos núcleos.

Las trazas del launcher están apagadas por defecto. Se recuperan con
`wine-launch --diagnostics programa.exe`; con ese modo se puede pasar WINEDEBUG
mediante el entorno. Los errores del launcher y del kernel siguen visibles.

Los ejecutables y scripts con ruta Unix arrancan en su carpeta: los instaladores
del CD pueden resolver `soccer\\install.exe` y otros archivos relativos. Se
conserva el directorio del launcher y de los servicios Wine. `.cmd` usa la misma
ruta `wine cmd /c` que `.bat`. TinyX arranca con `-dumbSched`, su planificador
alternativo, mientras la ABI Linux no implemente `setitimer`; no se simula un
temporizador exitoso ni se oculta un fallo de instalación.

El `INSTALL.BAT` de FIFA 96 llama a `soccer\\chkvesa.exe` y
`soccer\\install.exe`, ambos programas DOS. La corrección de rutas no incorpora
el entorno DOS/DPMI necesario para ese instalador y el juego.

## Verificación realizada

- Compilación de kernel, launcher, Mesa Linux, backend GLX y winex11 Unix.
- QEMU/KVM + virtio-vga-gl: el gate Linux dibujó un triángulo y verificó píxeles
  verdes y fondo negro; rechazó superficies de otro descriptor, dimensiones
  inválidas y un puntero inválido; pasó 140 ciclos de creación/cierre.
- Office 97 llegó al asistente y a la selección del grupo de programas. Se
  detectaron y corrigieron problemas de foco ICCCM y restauración del padre.
  No se completó ni se validó la instalación final después de esas correcciones.
- La validación Windows completa se interrumpió por pedido del usuario.
  Los probes `graphics-compat.c` y `heavy-compat.c` quedan para pruebas manuales;
  no se contabilizan como aprobados.
- FIFA96.EXE del ISO es DOS MZ/LE con DOS4GW. Requiere DOS/DPMI/emulación; no es
  una prueba Win32 Direct3D y todavía no se agregó un emulador DOS.

## Quake II con GPU en QEMU

`-vga virtio` sin la variante GL expone el dispositivo 2D. Para usar VirGL:

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-ata-user.img --gl --disk-interface ahci --cdrom media/installers/Quake2.iso --audio sdl
```

El script conserva KVM, dos CPU y 1536 MiB, usa `virtio-vga-gl` y `gtk,gl=on`,
y aplica el workaround local del módulo OpenGL de Ubuntu cuando hace falta.
`--disk-interface ahci` mantiene el disco de sistema en AHCI; el CD sigue IDE.

El swap GLX ahora resuelve el color GPU hacia el búfer que lee XPutImage,
sin descargar profundidad. Mesa respeta GL_REPLACE ignorando el color primario
al dibujar texturas. VirGL anuncia BLEND_MULTIPLY únicamente con soporte de
`dst.rgb *= src.rgb` y `dst.a *= src.a`; una prueba de píxeles verifica la
mezcla antes de registrar GFX3D. Esto acelera los lightmaps de Quake II con
GL_ZERO/GL_SRC_COLOR, conservando el fallback en otros drivers.

Las pruebas `test_glx_present.py` y `test_mesa_gpu_state.py` ejercitan las
funciones reales de presentación y traducción de estado. El diagnóstico
opcional `BLES_WINE_GL_TRACE=1`, en el entorno del proceso Wine, informa dibujos,
triángulos, transferencias y fallbacks cada 120 swaps; por defecto está apagado.
No se fuerza GPU para estados no representables ni se anuncia soporte GLSL.

Prueba Quake II: QEMU/KVM, virtio-vga-gl, AHCI, 2 CPU, 1536 MiB, Intel UHD 600
acelerada en el host; vid_ref=gl, ventana 640x480. El diagnóstico registró
120 swaps, 5236 llamadas GPU, 644377 triángulos, un upload de color, 120
resoluciones de color y cero fallbacks. La prueba del driver para mezcla
multiplicativa pasó. Estos contadores certifican uso de la ruta GPU en esa
secuencia; no son una medición de FPS ni una garantía de rendimiento para otros
mapas/efectos. Se actualizó y verificó la imagen de usuario con las bibliotecas
y driver compilados; el launcher de diagnóstico y su arranque automático no
se distribuyen.

## Corrección de paletas y costo de presentación (2026-10-08)

Las imágenes GL_COLOR_INDEX devuelven índices, no colores RGBA. El backend
nativo ahora expande las paletas RGB/RGBA antes de subir cada nivel mip o
subimagen, respeta la selección entre paleta compartida y por textura, e
invalida las imágenes cacheadas al editar su paleta. Las paletas de otros
formatos conservan la ruta software. Esto corrige las texturas rojas de Quake II.

Vincular nuevamente el mismo búfer/contexto ya no destruye el render target ni
la caché. El swap nativo usa la sincronización de su readback y evita un
segundo glFinish. Las capacidades del driver se conservan durante la vida del
render target, evitando consultarlas mediante ioctl en cada primitiva; la
búsqueda de texturas prueba primero la última entrada usada.

La ABI Linux mantiene pipes de 4096 bytes, incluida su atomicidad, y usa anillos
de 64 KiB para sockets. Ambos anillos pertenecen a la misma asignación de la
pareja y se liberan con ella. Así XPutImage deja de fragmentar cada cuadro de
640x480 en transferencias de sólo 4 KiB. La prueba IPC verifica pipes, SCM_RIGHTS,
ciclos de descriptores, poll/select/EOF/EINTR y 300000 bytes con wrap del anillo.

BLES_WINE_GL_TRACE=1 informa además texture_uploads y tiempos acumulados de
resolve/present/elapsed en milisegundos. elapsed incluye trabajo del juego entre
swaps; no debe interpretarse como tiempo exclusivo del driver. La presentación
actual todavía descarga color a RAM y envía píxeles por X11; rasterizado por GPU
no implica presentación sin copias. Las trazas y capturas de la prueba están en
ports/tinyx/evidence/quake2-virgl-palette-2026-10-08.

Resultado final: los primeros 120 swaps tardaron 6550 ms, con 980 ms de
resolución y 766 ms de envío X11; 5130 dibujos GPU, 668327 triángulos, 54 subidas
de textura y cero fallbacks. Antes de ampliar sockets y cachear capacidades,
la observación de 120 swaps dio 12446/939/5360 ms respectivamente. Equivale a
aproximadamente 10 frente a 18 FPS en esas secuencias de autoplay, con escenas
que avanzan según el tiempo: no es un timedemo determinista ni una garantía
para todos los mapas. La captura se tomó del búfer realmente resuelto por Mesa,
antes de X11, y muestra colores normales. La suite IPC terminó con estado 0.
Se verificaron byte a byte kernel, stage2, launcher normal y ambas bibliotecas
instaladas en la imagen de usuario; el arranque automático, volcado de cuadros
y trazas pertenecen sólo a la copia de diagnóstico.

## Mesa Gallium / VirGL programable

`make wine-gallium` construye y empaqueta Mesa 24.3.4 con SHA-256 fijado.
También se ejecuta automáticamente como dependencia de las builds normales
si faltan los archivos o cambiaron las fuentes. La caché predeterminada es
`build/wine/gallium`; `WINE_GALLIUM_CACHE=/ruta/persistente` permite conservarla
fuera de `build` al ejecutar `make clean`. Requiere compilador multilib, Ninja, Bison, Flex
y Python con venv. El frontend OSMesa usa exclusivamente el `pipe_screen`
VirGL de `virgl-winsys.c`: GLSL, FBO, texturas y dibujos se ejecutan en la GPU
del host mediante `/dev/blesgfx`. No se compilan softpipe ni llvmpipe.

La ABI `BLES_GFX_VIRGL` crea un contexto host por descripción de archivo,
aísla los recursos por propietario y espera fences reales del host. El cierre
libera contexto y recursos. El contexto del escritorio se mantiene separado.
Se admiten mipmaps y arrays; las transferencias son explícitas porque el
backing del kernel y la memoria del cliente no son coherentes entre sí.

El GLX local selecciona Gallium cuando el driver ofrece transporte VirGL y
está instalada `libbkvirgl.so.8`. `BLES_WINE_GL=virgl` permite seleccionarlo
explícitamente. Los drivers sin este transporte conservan Mesa 3.5; esta ruta
nueva no implementa Gallium sobre SVGA3D. La presentación descarga los píxeles
y los envía a TinyX; todavía tiene copias, aunque el renderizado sea por GPU.

`mesa-blesk-frontend.patch` agrega flags reales de contexto debug/forward,
búferes delantero y trasero independientes y copy-swap en GPU. El GLX presenta
los swaps y los flush/finish del búfer delantero; los FBO y el búfer trasero
no se presentan prematuramente. Las extensiones de creación se anuncian sólo
cuando están implementadas por la biblioteca cargada.

Pruebas:

```sh
python3 tests/desktop/test_virgl_transport.py
python3 tests/desktop/test_audio_bridge.py
```

`test-gallium.c`, ejecutado dentro del sistema, verifica GLSL 1.20, FBO con
píxeles esperados, contexto debug, textura compartida entre contextos, aislamiento
front/back, copy-swap y flush delantero. Pasó con VirGL sobre Intel UHD 600,
OpenGL 4.3. Los registros están en
`ports/wine/evidence/gallium-virgl-2026-10-08`. Esto verifica la ruta GPU;
la compatibilidad final de FIFA requiere comprobar menú y partida aparte.

El launcher acepta `--desktop=800x600` (u otra resolución válida) para usar
el escritorio virtual de Wine cuando una aplicación necesita cambiar de modo
sin modificar el escritorio del sistema.

El frontend selecciona formatos de profundidad/stencil admitidos por la GPU;
la prueba verifica profundidad de 24 bits y stencil de 8 bits con readback.
Los perfiles nuevos activan `EmulateModeset=Y`; al actualizar una imagen se
agrega sólo si no existe una preferencia explícita. Wine conserva sus cambios
de resolución emulados y compone el resultado dentro del escritorio nativo.

El parche Wine `0007-winex11-local-front-pixmap.patch` conserva el front buffer
resuelto por GPU en un pixmap persistente cuando el cliente está fuera de
pantalla. TinyX descarta dibujos a ventanas ocultas; el pixmap permite que la
composición GDI existente de Wine copie, recorte y escale esos píxeles. El
hook local de GLX es opcional y no cambia la rasterización a CPU. La prueba
`test-glx.c` verifica una ventana oculta y el contenido del pixmap resultante.


## Integración normal de FIFA y del frontend QEMU

GLX actualiza el tamaño del búfer al presentar cuando Wine cambia el tamaño
de una superficie sin volver a vincular el contexto. El pixmap persistente
del parche Wine 0007 y esta actualización forman parte de las fuentes normales;
0007 está incluido en `versions/wine-11.0.json`.

`run_qemu_virtio.py --gl` usa SDL/OpenGL por defecto. `--display gtk` permite
seleccionar GTK para diagnóstico. La validación gráfica también usa SDL para
VirGL. La prueba SDL mostró la introducción de FIFA sin errores EGL en el log
del host; el usuario confirmó que el juego abrió. Esto no certifica una partida
completa ni mide rendimiento.

Para conservar las aplicaciones instaladas al actualizar:

```sh
make user-update
```

La elección Direct3D de FIFA se conserva en su registro. Para seleccionar
`dx6` explícitamente en una instalación existente después de compilar:

```sh
python3 tools/update_user_image.py --fifa2000-gpu
```

El launcher de producción conserva las trazas apagadas y no abre FIFA al
arrancar. El autoarranque sólo se utilizó en una copia de diagnóstico.

## Perfil de rendimiento de FIFA 2000

La distribución y `make user-update` agregan `csmt=0` únicamente al perfil
Wine de `fifa2000.exe`, respetando un valor existente. Evita intercambios entre
el hilo del juego y el hilo de comandos mientras la ABI Linux comparte CPU1.
En una exhibición a 640×480 se observaron 0,63 presentaciones/s con el ajuste
predeterminado y 8,06 con este perfil. Todavía no es fluido; no se certificó un
partido completo ni el rendimiento con audio. Los intervalos, capturas y
limitaciones están en [la evidencia](../evidence/fifa2000-perf-2026-10-08/README.md).

Para optar explícitamente por el hilo de comandos, guardar `csmt` como DWORD 1
en `HKCU\Software\Wine\AppDefaults\fifa2000.exe\Direct3D`; los actualizadores
conservan esa preferencia. El juego debe volver a abrirse para leer el cambio.


El empaquetador prepara el perfil de distribución con el tema vigente, incluidos
OSS y los iconos, mediante `tools/wine_theme_defaults.py`. El helper lee la
importación y la versión del marcador desde el launcher, conserva los demás
valores y evita que la plantilla antigua v4 fuerce una importación durante el
primer warmup. Este paso sólo modifica el perfil nuevo de la imagen de
instalación; el actualizador conserva las preferencias del usuario.

La segunda ronda de rendimiento quedó detenida para revisar resultados.
El [balance, estado del disco y plan para retomar](../evidence/fifa2000-perf2-2026-10-08/CHECKPOINT.md)
distingue las mejoras GPU probadas, los experimentos descartados o desactivados,
el fallo de fence pendiente y la falta de un backend SVGA programable para FIFA
con Wine 11. Esta ronda no actualizó el disco habitual.


## Superficies independientes y pruebas Win32 (2026-10-15)

El frontend Gallium y GLX conservan los framebuffers por drawable y vinculan
superficies de lectura y dibujo independientes. Se verifica mediante GLX y
pbuffers WGL de Wine; el formato Gallium expone profundidad 24/stencil 8.
La reconstrucción corrige la dependencia libdrm faltante de la biblioteca
antes distribuida y comprueba el cierre ELF i386 al empaquetar.

La [evidencia de juegos Win32](../evidence/games32-2026-10-15/README.md)
contiene comparaciones antes/después, pruebas Direct3D/OpenGL y límites.
Las instrucciones y resultados anteriores conservan su valor histórico;
la nueva ronda valida DirectDraw y Direct3D 9 con píxeles reales, además
de los probes Linux. La aceleración programable sigue dependiendo de VirGL.

## Subidas y reutilización de búferes VirGL (2026-10-15)

FIFA 2000 mostró polígonos gigantes con fragmentos de otras texturas al usar
`TRANSFER_WRITE_ASYNC`. El ACK del host no equivale a una subida terminada en
GPU. El winsys vuelve a usar `TRANSFER_WRITE` con fence de finalización,
conservando las entregas asíncronas de comandos, las copias parciales de búferes
y las optimizaciones de presentación. La capacidad de subida asíncrona del
transporte ya no activa esa ruta en Wine.

También se detectó una carrera independiente en búferes reutilizados:
`TRANSFER_TO_HOST_3D` puede mapear su almacenamiento GL con
`GL_MAP_UNSYNCHRONIZED_BIT`. Un fence posterior a la escritura no protege los
vértices que un dibujo anterior todavía está leyendo. El winsys registra la
última secuencia de comandos que usó cada recurso y espera **antes** de
sobrescribir un búfer con usuarios pendientes. Si el búfer ya terminó, evita esa
espera adicional; si falla la espera, impide la escritura.

`tests/desktop/test_virgl_buffer_reuse.py` simula una GPU que consume los
vértices más tarde y verifica el orden, las subidas con fence y los errores.
`ports/wine/tests/gl-buffer-reuse.h` valida píxeles en GLX nativo y WGL/Wine:
reutilización de VBO, primeros uploads de recursos nuevos y mapas con descarte
mientras se acumulan dibujos. En Windows, las funciones OpenGL 1.1 se obtienen
de `opengl32.dll` cuando `wglGetProcAddress` no las proporciona.

`ports/wine/tests/direct3d7.c` agrega un caso de Direct3D clásico con 4096
quads texturados acumulados y lectura de los 1024 píxeles del destino. Lo
compila y ejecuta el gate `tests/kernel/run_wine_games32.py`, junto con las
pruebas anteriores de D3D9, D3D11, WGL y DirectDraw.

La evidencia de esta ronda se conserva en
`ports/wine/evidence/fifa2000-textures-2026-10-15/README.md`.

El parpadeo de FIFA 2000 persiste incluso con estas esperas. Esta ronda
verifica la carrera aislada de VBO; no certifica una corrección del juego
ni demuestra que las subidas asíncronas sean su causa.
