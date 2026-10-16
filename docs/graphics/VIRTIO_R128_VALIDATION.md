# Virtio/VirGL y ATI Rage128: corrección y validación

## Estado y alcance

La ruta común es Mesa → GFX3D → driver → compositor. No hay una excepción
para Tux Racer: las aplicaciones que usan el SDK/Mesa reciben los mismos
mipmaps, batching y reglas de sincronización. VMware conserva su protocolo y
no anuncia la nueva capacidad de envío ordenado.

**Las primeras mediciones de 28–35 FPS de Virtio quedaron invalidadas por
corrupción visual.** Los contadores de publicaciones y los primeros probes
no detectaban la reutilización de vértices que todavía estaba leyendo la GPU.
No deben usarse como evidencia de una versión estable.

## Causa de la corrupción

`RESOURCE_INLINE_WRITE` de VirGL puede mapear un VBO con
`GL_MAP_UNSYNCHRONIZED_BIT`. Entregar un `SUBMIT_3D` y recibir su respuesta sin
fence no significa que la GPU haya terminado de leer ese VBO. Reiniciar sus
posiciones en cada `vg_flush()` permitía que los vértices del siguiente draw
sobrescribieran los del anterior. El resultado podía incluir un atlas de
letras sobre toda la ventana, triángulos desplazados o el escritorio negro.

Ahora hay tres VBO de 256 KiB. Cada rango permanece reservado después del
envío. Al agotar el anillo, el driver envía todos los comandos pendientes,
espera un fence real y sólo entonces permite reutilizar sus rangos. No se
usa un hash como prueba de identidad de geometría ni se anuncia un fence
pendiente como completado.

La prueba `VBO lifetime` envía 64 lotes con posiciones y colores distintos,
sin esperas entre lotes, y comprueba 16 zonas al final. En QEMU, una copia
experimental con el reciclado anterior produjo **12 zonas incorrectas**;
la corrección produjo **0**. Las pruebas anteriores de triángulos repetidos
pasaban en ambos casos. Evidencia:

- `build/virtio-vbo-negative-run/serial.log`: reproducción deliberada; no distribuir.
- `build/virtio-vbo-verified/serial.log`: versión corregida.
- `build/virtio-visual-proof/visual-validation.mkv`: captura continua de la ventana.

## Resultado después de corregir el VBO

La versión `build/virtio-vbo-fixed/test.img` pasó la regresión y las pruebas
anteriores; completó Frozen River y cerró/reabrió Tux. Durante la carrera se
observaron intervalos de aproximadamente 21–23 FPS en este host Celeron N4020
con Intel UHD 600, QEMU 8.2.2 y KVM. Los intervalos de más de 100 FPS al llegar
a la pantalla de resultados **no son rendimiento de carrera**. Los contadores
reportaron cero readbacks en la ventana de juego. La segunda ejecución conservó una grabación a 2 FPS; se revisaron sus 128
cuadros del tramo de carrera, sin observar el fallo de atlas/triángulos ni
pérdida del escritorio. La captura está en `build/virtio-visual-proof/`.
Las imágenes normales ATA y USB contienen exactamente el DVR probado
(SHA-256 `28b33f3e87a5ce2314e9f73d8391a4c0f3a7d64964caebdc202407a78ba2caa5`).

## Otras correcciones de Virtio

- Los clientes pueden intercalar destinos; `begin/end` no reservan el motor
  para una aplicación durante toda la construcción de un frame.
- Los lotes grandes se dividen respetando triángulos y límites del protocolo.
- Alpha test con sus ocho comparaciones, DECAL conservando alpha, mipmaps
  nativos y selección independiente de filtro y repetición U/V.
- Coordenadas de perspectiva mediante `rhw`; composición premultiplicada con
  opacidad y recorte; clear de profundidad con valores intermedios.
- Las actualizaciones parciales no sobrescriben otros texels aunque la RAM de
  staging se haya usado previamente para otro mip.
- Readback y destrucción sincronizan el trabajo que usa los recursos.
- La presentación entrega la superficie VirGL al scanout sin readback del guest.

`GFX3D_CAP_ORDERED_SUBMIT` permite a Mesa diferir esperas al consumir resultados
en la misma cola GPU. `glFinish` sigue esperando el último fence; la presentación
GPU del SDK usa `glFlush`. El driver garantiza las esperas necesarias cuando
la CPU lee o destruye recursos. Los drivers que no anuncian la capacidad
conservan las esperas previas de Mesa.

## Repetir las pruebas

```sh
python3 tools/build_svga3d_validation.py --output build/graphics-test --perf
python3 tools/run_qemu_graphics_validation.py --image build/graphics-test/test.img --output build/virgl-run --record
python3 tools/check_qemu_graphics_log.py build/virgl-run/serial.log --race
python3 tools/summarize_graphics_perf.py --phase race build/virgl-run/serial.log
```

El nombre histórico del constructor incluye SVGA3D, pero su kernel de prueba
autodetecta el driver. Conserva símbolos y hashes de Tux, kernel y DVR. El
runner copia el disco, usa KVM, un vCPU, 256 MiB, Virtio VGA GL y GTK/OpenGL;
no modifica una VM personal. Guarda comandos, serial, errores del host y
capturas de la carrera y reapertura. El test incluye click, seis controles de
actividad y cierre/reapertura. No compilar mientras se mide.

Prueba de compatibilidad sin VirGL:

```sh
python3 tools/run_qemu_graphics_validation.py --image build/graphics-test/test.img --output build/virtio-2d-run --device virtio
python3 tools/check_qemu_graphics_log.py build/virtio-2d-run/serial.log --device virtio --race
```

`virtio-vga` solo ofrece 2D: el rasterizado 3D permanece en CPU. Para el uso
normal acelerado: `make run-virtio-gpu-gl`. El lanzador usa KVM si hay acceso
a `/dev/kvm`, informa cuando debe usar TCG y resuelve localmente el error de
carga `qemu_egl_display` de algunos paquetes Ubuntu, sin modificar paquetes.

Los FPS de `PERF:APP` cuentan publicaciones aceptadas, no refrescos físicos.
Revisar también `PERF:GUI`, readbacks, errores y la imagen durante la carrera.
El script de input conserva una ventana temporal después del click; si Tux
llega a la meta dentro de ella, sus FPS también incluyen la pantalla de resultados.
No presentar esa media como FPS de juego. Un `PASS` del analizador de logs no
sustituye la revisión visual. La carga
del host y la grabación afectan las mediciones.

## ATI

El trabajo de ATI está compilado y revisado contra el controlador histórico,
pero **no fue probado en una tarjeta física**. Los detalles, límites y pasos
para probar la Mobility M3/LF están en [ati-rage128-3d.md](ati-rage128-3d.md).
QEMU Virtio no valida el motor CCE de ATI.

## Referencias del protocolo

- [VirGL: escritura de recursos](https://android.googlesource.com/platform/external/virglrenderer/+/refs/heads/main/src/vrend_renderer.c): `vrend_renderer_transfer_write_iov`, mapeo no sincronizado de buffers.
- [VirGL: formato de comandos](https://android.googlesource.com/platform/external/virglrenderer/+/refs/heads/main/src/virgl_protocol.h).
- [Virtio 1.2, ciclo de comandos y fences](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
- [QEMU 8.2.2: temporizador de fences VirGL](https://github.com/qemu/qemu/blob/v8.2.2/hw/display/virtio-gpu-virgl.c): el polling diferido puede introducir esperas de 10 ms.
