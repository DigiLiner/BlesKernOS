# FIFA 2000, VirGL y VMware SVGA3D: segunda medición

Pruebas reales en el host Celeron N4020 de dos núcleos, Intel UHD 600,
KVM/SDL/OpenGL y VMware Workstation 26, con dos CPU y 1536 MiB por VM.
Se usaron copias temporales del disco. `result.json` conserva las cifras,
los archivos `*-wall.json` usan reloj monotónico del host y los logs conservan
los resultados de píxeles. Las trazas y el autoarranque sólo pertenecen a
los launchers de diagnóstico, no al launcher de las builds normales.

**Trabajo detenido a pedido del usuario.** Ver [el balance y plan](CHECKPOINT.md).
El disco habitual no se actualizó en esta ronda. TinyX bilinear quedó
experimental y apagado; se restauró el XFBDEV anterior del paquete.
La imagen `normal-check` fue construida antes de esa restauración: es sólo
un candidato de prueba y contiene el experimento.

## Cambios

- VirGL copia el rango modificado de buffers de vértices, conserva las texturas
  completas y negocia un envío sin fence cuando Gallium no solicita una fence
  pública. Los readbacks, transferencias y destrucciones mantienen el orden y
  esperan la finalización real. El ABI anterior conserva el envío síncrono.
- El staging de CPU es privado: mapearlo no espera a que termine la GPU;
  transferir su contenido sí sincroniza. Las consultas y fences públicas
  mantienen sus esperas. Un mutex ordena ioctls y contadores del mismo screen.
- El swap de Mesa copia back a front dentro del flush que drena los vértices:
  evita un segundo flush sin perder el contenido de back. GLX reutiliza su GC
  y conserva el upload a la ventana y al pixmap requerido por Wine.
- SVGA3D sella los bloques del staging de vértices con una fence por bloque;
  espera antes de reutilizar un bloque. Las fences públicas del compositor
  siguen existiendo. No se declara terminada una lectura pendiente.
- TinyX conserva como experimento, desactivado salvo `BLES_ENABLE_FAST_BILINEAR`, una ruta de bilinear para filas interiores ARGB/XRGB con
  transformación afín horizontal. Evita llamadas indirectas y comprobaciones
  por píxel, conserva fracciones y redondeo exactos, y usa el código general
  para bordes, clips múltiples, otros formatos y transformaciones.
- Las builds normales y `make user-update` incluyen estas bibliotecas, TinyX
  y VMWARESVGA3D.DVR. TinyX tiene dependencia de sus fuentes en el Makefile;
  deja de depender de reemplazar manualmente un binario precompilado.
  La integración del código está lista, pero esta ronda no actualizó el disco
  del usuario y el experimento bilinear no se distribuye activado.

## Benchmark de presentación X11

`ports/wine/graphics/bench-glx.c` crea su propio XFBDEV, renderiza 24 cuadros
por fase después de cuatro calentamientos y verifica el píxel del front.
La fase `present` incluye swap y XSync; mide recepción por X11, no refresco
visible del compositor nativo. No reproduce FIFA ni mide un techo absoluto.
Cada variante se ejecutó una vez; las fases cortas varían bastante.

| Backend a 640×480 | Lotes/cuadro | Antes, presentaciones/s | Después |
| --- | ---: | ---: | ---: |
| QEMU VirGL | 1 | 24,32 | 58,40 |
| QEMU VirGL | 128 | 14,34 | 26,97 |
| QEMU VirGL | 1024 | 23,52 | 24,37 |
| VMware SVGA3D clásico | 1 | 91,79 | 108,32 |
| VMware SVGA3D clásico | 128 | 90,40 | 87,05 |
| VMware SVGA3D clásico | 1024 | 77,77 | 79,82 |

En VMware el A/B usó el mismo disco no persistente y sólo cambió el DVR.
La mejora es pequeña y variable; no hay evidencia de una mejora universal.
Su glFinish clásico también resuelve el front, por eso la fase `render` no
se debe comparar como tiempo exclusivo de GPU contra VirGL.

## Partido y límites prácticos

Con `csmt=0`, nuevos drivers y TinyX anterior, un partido avanzando produjo
436 presentaciones en 44,39 segundos reales: **9,82/s**. Resolve, presentación
GLX y copia front acumularon 4,16, 4,64 y 2,02 segundos respectivamente;
no atribuir estos tiempos a todo el trabajo de XRender/TinyX/compositor.
La referencia de la [primera medición](../fifa2000-perf-2026-10-08/README.md)
fue 8–9/s, con otra cámara/momento y tiempos guest: no es un A/B idéntico.

El perfil durante el partido señaló `fbFetchTransformed`, el bilinear de
TinyX: Wine renderiza a 640×480 y lo amplía dentro de una ventana nativa.
Se ensayó `--desktop=640x480`, que eliminó la ampliación, pero no respondió
al teclado. El ensayo fue rechazado y no se activa por defecto.

El cuello restante combina traducción Wine/Direct3D 6, XRender por CPU,
readback/copias y ejecución de las tareas Linux en CPU1. La capacidad de
VirGL para presentar escenas simples por encima de 30/s demuestra que
9/s no es un techo fijo de la GPU emulada. TinyX publica hacia la GUI cada
33 ms, aproximadamente 30 Hz; el benchmark X11 puede superar ese refresco.
No se midió un límite de rasterizado exclusivo de la GPU ni un partido entero.

VMware pasó los selftests reales de superficies, color/depth, mezcla,
comparación alpha, mipmaps y vuelta del anillo de staging. Sin embargo,
expone **OpenGL 1.2 / Mesa 3.5 sin GLSL** en el backend clásico actual.
FIFA con Wine 11 falló allí (`vmware-fifa-before.log`): acelerar su FIFO no
agrega shaders. Falta implementar/conectar un backend Gallium SVGA
programable para poder validar FIFA acelerado en VMware.

## Validación

`virgl-bench-final.log` incluye GPU-PROBE y GLX-PROBE PASS: shaders, FBO,
depth/stencil, texturas compartidas, front/back, pixmap oculto, resize,
remapeo y actualización parcial repetida de VBO con draw pendiente.
Los logs de host correspondientes están vacíos, sin errores EGL/fence.
`virgl-bench-initial.log` falló un XGetImage fuera de la pantalla y se
excluye; el benchmark corregido verifica el front mediante un pixmap.
El paso intermedio `step1` no tiene medición host válida y se excluye.

Pruebas locales con ASan/UBSan, según el caso:

```sh
python3 tests/desktop/test_virgl_transport.py
python3 tests/desktop/test_virgl_ranges.py
python3 tests/desktop/test_virgl_async.py
python3 tests/desktop/test_svga_vertex_ring.py
python3 tests/desktop/test_glx_present.py
python3 tests/desktop/test_tinyx_bilinear.py
```

También pasaron las pruebas de estado Mesa, defaults por aplicación y bridge
de audio. Los tests con host simulado retrasan deliberadamente las lecturas
para comprobar que las optimizaciones no reutilizan memoria antes de tiempo.

Los ensayos `tinyx-first-fence-failed`, `audio-host-fence-failed` y
`noaudio-repeat-fence-failed` quedan excluidos del rendimiento: QEMU/VirGL
registró `Failed to create fence sync object`, seguido de timeout de lectura
en el guest; uno terminó en fallo de FIFA. La prueba con audio también
registró errores de volumen/mute de PulseAudio. No certifica audio funcional
ni FPS con sonido. La comparación con TinyX anterior avanzó hasta el menú sin fallo en el
intervalo observado (`previous-tinyx-isolation`), pero se detuvo antes de
validar otro partido. No alcanza para establecer la causa. No se
atribuye automáticamente al reescalado ni se considera arreglado por SDL.
