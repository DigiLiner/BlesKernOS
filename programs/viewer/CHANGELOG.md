# Changelog

## Viewer 3.2 - streaming AVI y MPEG-1

- MPEG-1 PS: callbacks de lectura/seek/tell y buffers de `pl_mpeg`;
  ya no necesita el archivo comprimido completo en RAM.
- AVI RIFF: lectura de metadatos, índice de chunks y frame activo bajo demanda.
  Audio PCM AVI: caché de lectura 4 KiB.
- Camino de renderizado XRGB opaco (`bk_gui_surface_draw_xrgb`) y opt-in
  para ventana streaming. La aceleración final depende del compositor GPU.
- Conversor de compatibilidad para entradas MPEG-2 Video, MP3 y AVI con
  codecs no nativos (`tools/convert_media.sh`, ejecutado en el host).
- No se añaden decodificadores nativos MPEG-2 ni MP3; soporte nativo
  MPEG-1 Video/MP2 y AVI MJPEG/DIB/PCM permanece.
- GIF/BKV: lectura completa limitada a 32 MiB desde la interfaz.
- Prueba externa de lectura/seek MPEG y AVI (incluido PCM), con sanitizadores.

## Viewer 3.1

- Soporte real para `.MPG`/`.MPEG` en contenedor MPEG-1 Program Stream.
- Decoder MPEG-1 Video con imágenes I/P/B mediante `pl_mpeg` MIT.
- Decoder MPEG-1 Audio Layer II (MP2).
- Conversión MP2 estéreo a PCM unsigned 8-bit mono para la API de BlesKernOS.
- MP2 a 48 kHz se reduce 2:1 a 24 kHz para mantener compatibilidad con SB16;
  32 y 44,1 kHz conservan su frecuencia.
- Estado MPEG de vídeo y audio separado para que seek/frame-drop no desincronicen
  ni destruyan el cursor del otro stream.
- Conteo de picture start codes del stream de vídeo para timeline/frame count
  real en vez de estimarlo sólo por duración.
- Framerates MPEG exactos, incluidos 24000/1001, 30000/1001 y 60000/1001.
- Ruta full-frame XRGB para MPEG-1 en los perfiles de calidad apropiados.
- Integración freestanding de `pl_mpeg` con `bk_sys_alloc/free`; sin libc.
- Build fijado a i486/x87 y sin SSE/SSE2 para no heredar instrucciones modernas
  del compilador host.
- Dependencia fijada al commit `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023`
  y verificación del Git blob al descargarla.
- UI, mensajes y documentación actualizados a Viewer 3.1.

## Viewer 3.0

- Reproducción AVI Motion-JPEG (`MJPG`/`JPEG`) usando el decoder JPEG público de
  BlesKernOS.
- Ruta full-frame XRGB con `bk_gui_surface_draw_image()` para conservar la
  calidad de vídeo fotográfico y reducir llamadas GUI.
- AVI DIB raw y GIF pueden usar también la ruta full-frame en calidad alta.
- Audio AVI PCM 8/16-bit, mono/estéreo, 4-44,1 kHz.
- Conversión PCM a unsigned 8-bit mono para la API nativa de BlesKernOS.
- Play/pause/stop/seek/loop sincronizan el cursor de audio con el vídeo.
- Volumen y mute.
- Menús nativos del SDK en lugar de una barra de menú dibujada manualmente.
- Selector de archivos nativo para `File -> Open...` y `Ctrl+O`.
- Rediseño retro: visor hundido, panel de transporte, display de tiempo,
  timeline, volumen, indicadores y status bar.
- Nuevo slider y display inset en `retro_ui`.
- Carga de iconos compartida para evitar fugas por recarga de medios.
- Apertura transaccional: un archivo inválido no destruye el medio ya abierto.
- Limpieza de recursos reforzada ante AVI incompletos/corruptos.
- El SDK patch sólo declara las funciones PCM que el loader ya exportaba; no
  requiere syscall ni driver nuevo.
- Eliminadas dependencias accidentales de helpers 64-bit de libgcc.

## Viewer 2.0

- Nuevo BKV2 TileDelta con keyframes y delta de tiles RGB565.
- Encoder BKV2 hasta 1920x1080 y 120 FPS.
- Compatibilidad BKV1 conservada.
- Decoder AVI RIFF para DIB raw 8/16/24/32-bit (BI_RGB/BI_BITFIELDS).
- Decoder GIF87a/GIF89a + LZW, interlace, transparencia y disposal.
- Renderer adaptativo: Auto, Exact, Fine, Fast, Turbo y Ultra.
- Auto sube hasta bloques de 32 px para contenido de 90+ FPS.
- Frame skipping para mantener el reloj cuando la máquina no alcanza.
- Contador de frames saltados.
- Seek no lineal en BKV2 desde keyframe y reconstrucción correcta de GIF.
- Fuente máxima del decoder aumentada a 1920x1080.
- Se eliminó cualquier dependencia accidental de `__udivdi3`/libgcc.
- Sigue usando únicamente símbolos públicos `bk_*` de la API Ring 3.

## Viewer 1.0

- BKV1 RGB565/RLE.
- Reproducción, pausa, loop y timeline.
- GUI retro básica.
