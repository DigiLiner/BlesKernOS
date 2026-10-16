# Optimización y pruebas — 2026-10-13

Cambios incorporados al código y a `rootfs/WINEGATE/bin/XFBDEV` del paquete.
Imagen lista para probar: **`build/bleskernos-wine-optimized.img`**. Conserva
el prefijo y los archivos de la imagen base; actualiza kernel/Stage 2, Vision,
mouse PS/2 y launchers junto con TinyX. No contiene los launchers de diagnóstico
ni autoinicios de pruebas. La imagen habitual no se modificó.

## Resultado medido

QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, VGA VMware a 800×600, VMs de a una.
Comparación de reescalado bilinear real en TinyX, 640×480 → 800×600, 80
composiciones por formato con XSync. Tiempos host monotónicos de recepción
de los marcadores seriales. Es una carga sintética de XRender, **no FPS de
FIFA ni refresco visible de Vision**.

| Formato | Antes | Optimizado | Relación | Hash de salida en ambos |
| --- | ---: | ---: | ---: | --- |
| RGB | 41,40 composiciones/s | 73,42 composiciones/s | 1,77× | `b4a29ac2` |
| ARGB | 43,44 composiciones/s | 66,58 composiciones/s | 1,53× | `e1e9a4b3` |

Son muestras cortas; la carga y frecuencia del host influyen. Las pruebas
de matemáticas verifican además todos los valores de fracción subpíxel,
millones de píxeles, alfa, pasos positivos/negativos, clips y guardas.

En escritura, una comparación exploratoria de 16 teclas dio mediana
294,08 → 281,45 ms y máximo 796,24 → 443,14 ms. **No demuestra que la demora
al escribir esté resuelta.** El sondeo QMP del framebuffer introduce coste
y perturbación; las lecturas duraron de mediana 7,06/8,12 ms y las colas
pueden variar. No se presenta el descenso del máximo como una garantía de p95.

La lectura adicional de las colas en una copia de la imagen final mostró
que TinyX consumía la entrada en unos 25–35 ms de mediana, mientras la nueva
imagen de la ventana llegaba bastante después. Esto acota el trabajo pendiente
al tramo posterior a la entrada de TinyX y anterior a la publicación del contenido,
que incluye Wine/GDI, X11 y las copias. Las sondas de etapas agregan consultas
QMP; sus valores no se mezclan con el A/B de 16 teclas.

FIFA y Photoshop no se ejecutaron: el usuario confirmó haber perdido sus
imágenes con esas instalaciones. No se extrapolan las cifras sintéticas a esos
programas ni se reemplazan las mediciones históricas de FIFA.

## Cambios conservados

- `fbcompose.c` y `fb_bles_bilinear.h`: la ruta rápida anterior exigía que
  **toda** la fila fuese interior. La ampliación de una imagen completa toca
  sus bordes y descartaba la ruta rápida. Ahora se separa el tramo interior;
  los extremos usan el algoritmo general. Se conservan fracciones, redondeo,
  alfa y fallback para otros formatos, transformaciones y clips múltiples.
  Se habilita esta ruta en BlesKernOS.
- `bles_present.h` y `blesos.c`: la detección exacta con memcmp sustituye los
  dos hashes por píxel. También detecta escrituras directas/compartidas que
  no notifican X Damage. La imagen a enviar se congela antes de ioctl y la
  referencia sólo se confirma si la publicación tiene éxito: una escritura
  concurrente no queda absorbida en una referencia nunca publicada. OOM
  conserva publicación completa y los fallos dejan el cuadro pendiente.
  Se agrega un buffer de 4 bytes por píxel, liberado al finalizar.
- `kernel/linux/i386_ipc.inc` y `blesos.c`: el descriptor gráfico expone
  disponibilidad de entrada mientras su cola tenga datos. KDrive lo observa
  con select junto a los sockets X11 y procesa la entrada al despertar.
  Se conserva el sondeo existente como respaldo. No se quita la restricción
  de Wine a CPU1 ni se cambia su ABI de estructuras.

Se conserva el intervalo de publicación de 33 ms y el sondeo base de 20 ms.
El ensayo de aumentar globalmente su frecuencia no produjo una mejora estable
y se descartó. También se descartó un intento basado sólo en Damage: perdió
actualizaciones de texto. Esas variantes no están en el paquete ni en la imagen.

## Validación

- `tests/desktop/test_tinyx_present.py`: ASan/UBSan, escrituras en cada byte,
  límites exactos con página protegida, reintentos, resize/OOM/reset y modificación
  del buffer vivo después de congelar el envío.
- `tests/desktop/test_tinyx_bilinear.py`: ASan/UBSan, colores exactos y selección
  de interiores, incluidos los bordes que rechazaban antes la ampliación completa.
- `tests/kernel/test_gfx_poll.py`: cola vacía/con datos/drenada/wrap, solicitudes
  de escritura, contexto inexistente, dispositivos comunes y EOF de sockets.
- QEMU real: XRender antes/después con hashes iguales; Notepad con 16 teclas,
  copiar/pegar, reinicio de Vision y nueva escritura/portapapeles después.
- La imagen final también arrancó en una copia y recibió ocho teclas en las
  sondas de entrada/publicación. BFS4 y los bytes de los payloads instalados
  se verificaron; kernel, Stage 2, Vision, PS/2 y launchers compilaron.

## Reproducir

```sh
python3 tests/desktop/test_tinyx_present.py
python3 tests/desktop/test_tinyx_bilinear.py
python3 tests/kernel/test_gfx_poll.py
python3 tests/kernel/run_xrender_bench.py --out /tmp/nuevo-render-test \
  --tinyx ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/bin/XFBDEV
python3 tests/kernel/run_wine_gui_smoke.py \
  --base build/bleskernos-wine-optimized.img --out /tmp/nuevo-wine-ui-test \
  --ram 1536 --vga vmware --wine-debug=-all --latency-samples 16
```

Usar directorios nuevos y una sola VM a la vez. `--input-probe` agrega sondas
de las colas y de publicación; `--latency-only` omite el reinicio funcional.
Los tests trabajan sobre copias. Los JSON, logs, capturas, parche del código
de producción y hashes de artefactos se conservan en este directorio.

La comparación anterior de 150 s sin ventana mezclaba otro kernel/entorno con
componentes anteriores de la imagen. Las pruebas sincronizadas posteriores sí
abrieron Notepad. Aquel ensayo no se utiliza para atribuir una mejora de arranque
a esta optimización ni para afirmar que se arregló un deadlock del kernel.
