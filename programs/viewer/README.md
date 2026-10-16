# Viewer 3.2 para BlesKernOS

Viewer es un reproductor multimedia de terceros para BlesKernOS. Usa únicamente
la API pública Ring 3 del SDK para ventanas, menús, selector de archivos,
imágenes, audio y memoria. La interfaz conserva el diseño de reproductor de
mediados de los 90 introducido en Viewer 3.0.

## Viewer 3.2: reproducción con memoria acotada (MPEG-1 y AVI)

Esta actualización abre archivos **MPEG-1 PS** y **AVI RIFF tradicionales**
mediante lectura aleatoria y buffers de trabajo, sin `bk_file_read_all()`:

- MPEG-1: `pl_mpeg` lee por callbacks conectados a `bk_file_read/seek/tell`;
  cursores separados para escaneo inicial, vídeo y MP2.
- AVI: analiza el encabezado (límite 1 MiB), crea índices compactos de
  fotogramas/audio, y carga sólo el frame activo (límite 16 MiB) y audio en
  caché de 4 KiB. Esto incluye MJPEG, DIB/PCM de los mismos formatos anteriores.
- La GUI solicita `bk_gui_window_set_streaming()` y presenta los frames opacos
  mediante `bk_gui_surface_draw_xrgb()`. El compositor/driver decide si el blit
  y el escalado pueden acelerarse; no hay *decodificación MPEG por GPU* ni
  uso directo de una superficie GPU desde Viewer.
- GIF/BKV siguen usando lectura completa, limitada a 32 MiB en Viewer.
- Los archivos AVI OpenDML multidsegmento/AVIX y RIFF no estándar todavía no
  están implementados en la ruta streaming.

### MPEG-2, «MPEG-3» y compatibilidad ampliada

**No hay decoder MPEG-2 Video nativo ni MP3 (MPEG Audio Layer III) en Viewer.**
MPEG-3 como estándar de vídeo independiente se abandonó (no confundir con MP3).
`pl_mpeg` sólo decodifica MPEG-1 Video y MP2. Una película con MPEG-2/MP3,
DivX/Xvid u otros codecs se puede convertir en el equipo host a MPEG-1 PS/MP2:

```sh
sh tools/convert_media.sh "pelicula-con-mpeg2-o-mp3.mpg" "pelicula-bles.mpg" retro
sh tools/convert_media.sh "video-original.avi" "video-bles.mpg" quality
```

Este proceso requiere `ffmpeg` instalado en el PC anfitrión, **no en BlesKernOS**.
La salida `retro` usa 352×288 25 fps (900 kbit/s MPEG-1, MP2 44,1 kHz).
La salida `quality` usa 640×480 30 fps (1800 kbit/s MPEG-1). Ambos mantienen
la proporción de la imagen y agregan bordes si es necesario.
No se afirma reproducción nativa de los formatos de entrada.

## MPEG-1 `.MPG` / `.MPEG`

Viewer 3.2 abre **MPEG-1 Program Stream (MPEG-PS)** real:

- Vídeo: **MPEG-1 Video** con imágenes I, P y B.
- Audio: **MPEG-1 Audio Layer II (MP2)**.
- Demultiplexado de Program Stream y decodificación mediante `pl_mpeg`, fijado
  a un commit concreto y adaptado al entorno freestanding de BlesKernOS.
- Conversión YCbCr 4:2:0 a XRGB para presentar frames completos mediante la API
  gráfica pública del OS.
- Seek de timeline y salto de frames.
- El estado del decoder de audio es independiente del de vídeo, para que un
  seek de imagen no destruya la posición MP2.

Viewer detecta MPEG por la cabecera MPEG-PS, no por la extensión. Un archivo
renombrado a `.MPG` que contenga otro codec no se acepta.

### Audio MP2 y BlesKernOS

MPEG-1 Layer II usa normalmente 32, 44,1 o 48 kHz. Viewer convierte el estéreo
MP2 a PCM unsigned 8-bit mono para la API pública del OS:

- 32 kHz -> PCM 32 kHz.
- 44,1 kHz -> PCM 44,1 kHz.
- 48 kHz -> PCM 24 kHz mediante reducción 2:1.

El último caso existe porque el backend SB16 actual de BlesKernOS acepta hasta
44,1 kHz, mientras AC'97 y Maestro 3 llegan a 48 kHz. De esta forma un MPEG con
audio de 48 kHz sigue teniendo sonido también con SB16.

## Otros formatos soportados

### AVI

- **Motion-JPEG (`MJPG` / `JPEG`)**: cada frame se decodifica con
  `bk_gui_image_decode_jpeg()` de BlesKernOS.
- **DIB sin comprimir**: 8, 16, 24 y 32 bits; BI_RGB y BI_BITFIELDS.
- **Audio PCM**: formato WAVE PCM 1, 8 o 16 bits, mono o estéreo, 4-44,1 kHz.

### GIF

- GIF87a / GIF89a animado.
- Paleta global/local, transparencia, interlace, delays y disposal 0/1/2/3.

### BKV

- **BKV2 TileDelta** con keyframes y tiles RGB565.
- **BKV1 RGB565/RLE** para compatibilidad.

## Lo que `.MPG/.MPEG` significa en Viewer 3.2

Soportado:

```text
MPEG Program Stream
  video: MPEG-1 Video
  audio: MPEG-1 Layer II (MP2), opcional
```

No soportado por esta ruta:

- MPEG-2 Video / DVD VOB.
- Transport Stream `.TS`.
- MPEG-4 ASP.
- H.264/AVC o H.265/HEVC.
- MP3, AAC o AC-3.
- MP4/MOV/MKV/WebM.
- Elementary stream `.M1V` sin contenedor Program Stream.

Por lo tanto Viewer no intenta adivinar codecs ni muestra compatibilidad sólo
por la extensión del archivo.

## Calidad y rendimiento

Viewer mantiene las dos rutas de presentación de 3.0:

1. **Full-frame XRGB** para vídeo fotográfico y MPEG-1 cuando el perfil y el
   tamaño lo permiten. En `Auto`, MPEG-1 usa esta ruta hasta aproximadamente
   500.000 píxeles de salida y 30 FPS.
2. **Renderer adaptativo por bloques** para máquinas lentas y perfiles
   `Fast`, `Turbo` y `Ultra`. BKV2 conserva además su ruta nativa por tiles.

Perfiles: `Auto`, `Exact`, `Fine`, `Fast`, `Turbo` y `Ultra`.

El límite de apertura sigue siendo 1920x1080, pero no implica que una PC de los
90 pueda reproducir esa resolución en tiempo real. Para MPEG-1 dentro del
contexto de BlesKernOS son objetivos mucho más razonables 320x240, 352x240,
352x288 o 640x480 a 15-30 FPS.

## Interfaz y controles

Viewer usa menús reales del SDK y el selector nativo de archivos. Incluye visor
hundido, transporte 3D, display de tiempo, timeline, volumen, mute, loop,
selector de calidad y barra de estado.

Controles:

- `|<`, `<`, `>`, `>|`: primer, anterior, siguiente y último frame.
- `Play/Pause` y `Stop`.
- Timeline con seek directo.
- Volumen y mute.
- Loop y selector de calidad.

Atajos:

- `Space` / `P`: play-pause.
- `R`: volver al comienzo.
- `L`: loop.
- `Q`: cambiar calidad.
- `[` / `]`: frame anterior/siguiente.
- `Ctrl+O`: abrir archivo.

## Dependencia MPEG

Viewer 3.2 usa `pl_mpeg` de Dominic Szablewski, bajo licencia MIT, para el
demux MPEG-PS, MPEG-1 Video y MP2. El build fija exactamente:

```text
c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
```

`make` descarga `third_party/pl_mpeg.h` sólo si falta y verifica el Git blob
esperado antes de usarlo. Esto evita depender de una versión flotante de la
librería. El adaptador `lib/pl_mpeg_bles.c` reemplaza malloc/realloc/free y las
operaciones de memoria de libc por implementaciones basadas en `bk_sys_alloc()`
y `bk_sys_free()`; no se enlaza libc dentro de `VIEWER.O`.

Si la máquina de compilación no tiene Internet, descarga manualmente
`pl_mpeg.h` del commit indicado y colócalo en `third_party/pl_mpeg.h`.

## Compilar

Viewer es un programa predeterminado y vive en `programs/viewer`. El Makefile principal lo compila como `build/programs/viewer.o` y la imagen lo instala en `/SYSTEM/PROGRAMS/VIEWER.BEX`:

```sh
make -C "programs/viewer" ROOT=../..
```

El resultado es:

```text
programs/viewer/build/VIEWER.O
```

El decoder se compila explícitamente para `i486`, x87 y sin SSE/SSE2, igual que
el objetivo retro del OS; no depende de instrucciones modernas del host de
compilación.

Utiliza las funciones PCM del SDK público del sistema.

## Ejecutar

```text
VIEWER.O /ata0/VIDEOS/INTRO.MPG
VIEWER.O /ata0/VIDEOS/CLIP.MPEG
VIEWER.O /ata0/VIDEOS/INTRO.AVI
VIEWER.O /ata0/VIDEOS/ANIM.GIF
VIEWER.O /ata0/VIDEOS/CLIP.BKV
```

También puede iniciarse vacío y usarse `File -> Open...`.

## Crear un MPEG-1 compatible desde un vídeo moderno

Perfil recomendado, cercano al uso clásico de MPEG-1 y liviano para el OS:

```sh
ffmpeg -i entrada.mp4 \
  -vf "scale=352:288:force_original_aspect_ratio=decrease,pad=352:288:(ow-iw)/2:(oh-ih)/2" \
  -r 25 -c:v mpeg1video -q:v 4 \
  -c:a mp2 -b:a 128k -ar 44100 -ac 2 \
  -f mpeg SALIDA.MPG
```

Para probar más carga de CPU puede subirse la resolución, pero 352x288/25 FPS
es una mejor base para validar primero vídeo, B-frames, seek, sincronización y
audio MP2.

El comando AVI de Viewer 3.0 continúa siendo válido:

```sh
ffmpeg -i entrada.mp4 -vf "scale=640:-2,fps=24" \
  -c:v mjpeg -q:v 4 -pix_fmt yuvj420p \
  -c:a pcm_u8 -ac 1 -ar 22050 SALIDA.AVI
```

## Arquitectura

```text
viewer.c            ventana, menús, transporte, timeline y audio
lib/video.c         fachada común y helpers de tiempo
lib/mpeg.c          MPEG-PS/MPEG-1/MP2 e integración con Viewer
lib/pl_mpeg_bles.c  adaptación freestanding del decoder MPEG
lib/avi.c           AVI RIFF, DIB, Motion-JPEG y PCM
lib/gif.c           GIF + LZW
lib/bkv.c           BKV1/BKV2
lib/render.c        full-frame XRGB + renderer adaptativo/tile
lib/retro_ui.c      controles 3D estilo 90s
tools/fetch_pl_mpeg.py  descarga reproducible de la dependencia MPEG
```

## Limitaciones conocidas

1. **Los AVI/MPEG-1 no cargan todo el archivo en RAM**; el índice AVI y los
   buffers del decodificador siguen consumiendo memoria. GIF/BKV conservan
   lectura completa, con límite de 32 MiB. El AVI indexador hace un recorrido
   de headers de chunks (dos pasadas), por lo que abrir AVI muy grandes puede
   requerir I/O notable.
2. **MPEG-1 se decodifica por CPU.** La presentación XRGB puede beneficiarse
   del compositor/gráfico acelerado, pero no se garantiza descarga a GPU.
3. **El audio PCM del OS no ofrece timestamps/sample-accurate queueing.** Viewer
   sincroniza por reloj, buffers y reposicionamiento al hacer seek. `pl_mpeg`
   puede leer PTS para demux/seek, pero su reproducción normal no los usa como
   reloj continuo de sincronización; un Program Stream con multiplexado poco
   regular todavía puede mostrar deriva A/V.
4. **La primera apertura de un MPEG recorre el stream de vídeo para contar
   imágenes**, lo que permite una timeline con número real de frames pero añade
   trabajo inicial en archivos grandes.
