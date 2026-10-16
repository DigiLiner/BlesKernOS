# FIFA 2000 y Quake II: reparación y presentación i386

Trabajo sobre copias de `build/bleskernos-wine-games32.img`, que ya contiene las instalaciones del usuario. La imagen de entrada se conserva. Entorno: QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, disco AHCI, CD IDE, virtio-vga-gl y EGL headless sobre renderD128. Wine 11.0, Gallium 24.3.4. Exclusivamente programas Windows de 32 bits.

## Hallazgos reproducidos

- En la instalación de FIFA faltaba `C:\Program Files\EA Sports\FIFA 2000\fifa2000.exe`; estaban los datos y los accesos directos. El diagnóstico Win32 no pudo abrir el EXE instalado, pero sí el del CD. No se ha aislado por qué el instalador lo omitió.
- El original del CD usa SafeDisc. La herramienta de reparación restaura el original verificado y, con `--use-disc-executable`, usa la variante que ya existe en esta ISO del usuario. Guarda el original como `.safedisc-backup`. Esto no implementa SafeDisc ni distribuye un ejecutable del juego.
- La copia reparada con `Thrash Driver=dx6` llega a selección de equipos, selección de lados, carga y partido con Direct3D. El partido de referencia permaneció activo unos 13 minutos, sin fallos de fence/EGL del host. Hubo polígonos negros durante la cámara de introducción; la captura posterior del juego muestra el campo y los jugadores. No se ha aislado ese defecto de introducción.
- Quake II está instalado en modo mínimo: no tiene `baseq2/pak0.pak` local. Su acceso directo contiene `+set cddir D:\install\data +set basedir C:\Quake2`. Ejecutar sólo el EXE no reproduce correctamente esa instalación: produce `Couldn't load pics/colormap.pcx`.
- El probe Win32 reconoce D: como CD-ROM (tipo 5), abre el ejecutable original y lee la cabecera PACK del archivo de 183997730 bytes del CD. No se atribuye ese error a una falta de lectura ATAPI.

## Cambios

- `tools/repair_fifa2000.py` permite restaurar un EXE ausente desde la ISO verificada y elegir explícitamente `--renderer dx6`. Conserva el registro, preferencias e idioma; rechaza un EXE desconocido y publica la imagen reparada sólo tras comprobarla.
- Quake II desborda el búfer de impresión de su lista de extensiones OpenGL moderna (escritura fuera de la pila en `00431fb3`, seguida de un handler SEH corrupto). Se agregó un perfil `AppDefaults\quake2.exe\OpenGL\EnabledExtensions`, con las extensiones clásicas del renderer. Usa el [filtrado nativo por aplicación de Wine 11](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/opengl32/unix_wgl.c), conserva preferencias explícitas y no limita OpenGL para otros ejecutables. La [documentación de Mesa](https://docs.mesa3d.org/application-issues.html#old-opengl-games) describe esta clase de incompatibilidad; el perfil local actúa en Wine porque sus contextos modernos reconstruyen la lista mediante `glGetStringi`.
- El launcher abre los `.lnk` mediante `wine start /wait /unix`, conservando los argumentos y directorio que resuelve Wine. Espera a la aplicación antes de cerrar TinyX.
- GLX evita volver a vincular el mismo contexto y las mismas superficies cuando su geometría validada no cambia.
- Wine comunica explícitamente el modo de composición fuera de pantalla mediante `bles_glx_set_offscreen`. Se resuelve el color GPU y se conserva la copia al pixmap de composición, pero se omite el envío XPutImage redundante al cliente. Al volver al modo directo se reanuda el envío. Esto evita inferir visibilidad desde `map_state`: Wine puede mantener el cliente mapeado bajo un padre vacío. Las versiones anteriores de libGL siguen funcionando con el driver nuevo, sin esta optimización, y el driver anterior conserva su comportamiento con libGL nuevo.
- Se corrige `mprotect` sobre bloques de Wine con muchos commits paginados: se confundían con reservas sin memoria y se volvía a recorrer recursivamente la tabla completa por cada segmento. El pager verifica cobertura y pins antes de cambiar permisos y protege sus segmentos en dos recorridos, manteniendo COW y los huecos.
- El límite de metadatos VMA sube de 1024 a 8192, con crecimiento bajo demanda desde 64. Quake II alcanzó 1024 durante la carga y falló asignaciones aunque quedaba RAM. La tabla completa ocupa 192 KiB; no se reserva de entrada. El test guest ejercita 1537 mapas, 128 commits graduales de un bloque y cambios de permisos sobre el bloque completo, fork, aislamiento, reutilización y tres ciclos de liberación: PASS, sin pérdida de frames ni páginas virtuales.
- Las subidas VirGL negocian `ASYNC_UPLOAD`: el host consume los datos CPU antes de confirmar, pero la llamada no espera una fence GPU por cada mip. Se conservan las barreras en lectura, espera explícita, fences públicas y destrucción. Los kernels anteriores siguen usando la subida síncrona. La [cola de comandos de QEMU 8.2.2](https://github.com/qemu/qemu/blob/v8.2.2/hw/display/virtio-gpu-virgl.c) confirma las operaciones sin fence tras procesarlas y consulta las fences pendientes con un timer de 10 ms; esa espera repetida era un costo grande en las cargas de texturas.
- El escalado bilineal de TinyX usa SSE2 cuando la CPU lo admite y conserva la implementación escalar para procesadores anteriores. Mantiene exactamente los píxeles, alfa y precisión de la ruta anterior.

## Pruebas y límites

Las pruebas de host pasan para persistencia y cambios de superficies GLX, rollback, resize, destrucción diferida, presentación oculta/remapeada y restauración de FIFA. El escalador pasa comparaciones con referencia independiente y ASan/UBSan tanto en i386 como en host. El microbenchmark del escalador no es una medición de FPS del juego.

La suite guest final pasa DirectDraw, OpenGL 4.3, WGL y Direct3D 9/11 (shaders, textura, profundidad y Reset). Incluye dibujar rojo, actualizar la misma textura a verde sin `glFinish` y dibujar a otra región: el readback conserva rojo/verde. La lista OpenGL del ejecutable de prueba mantiene 7213 bytes; Quake II recibe 342 bytes (357 con el prefijo de log), de modo que el perfil no reduce las capacidades globales. No hubo errores de fence/EGL durante esa suite.

Las mediciones GLX cuentan presentaciones producidas por el juego, no refrescos visibles del monitor. Las escenas de FIFA no son un replay determinista; no permiten prometer una tasa estable para todos los partidos. La referencia, tras descartar el primer intervalo afectado por trabajo de compilación, dio 10,04 y 10,35 presentaciones por segundo host. El pipeline todavía descarga color y lo envía por X11; no hay presentación sin copias.

## Reproducción

```sh
python3 tests/desktop/test_fifa_repair.py
python3 tests/desktop/test_glx_drawables.py
python3 tests/desktop/test_glx_present.py
python3 tests/desktop/test_tinyx_bilinear.py
```

Para reparar una copia detenida de la instalación, usando esta ISO concreta:

```sh
python3 tools/repair_fifa2000.py --image build/mi-copia.img \
  --iso media/installers/FIFA2000.iso --use-disc-executable --renderer dx6
```

`tests/kernel/run_wine_game.py` crea su propia copia y registra QMP, serial y comando. `tools/wine_guest_control.py DIRECTORIO --capture nombre` captura el framebuffer nativo incluso bajo EGL headless; también admite `--key ret`, `--click X Y` y `--quit`. `tools/measure_wine_frames.py DIRECTORIO --seconds 20 --samples 3` registra intervalos sin pausar la máquina.

La imagen de prueba final se prepara como `build/bleskernos-wine-tuned.img`. Incluye `C:\Quake2\PLAY-GL.CMD`, que conserva la ruta de datos de la instalación mínima en D: y abre el renderer OpenGL en ventana; montar `Quake2.iso` para usarlo. No copia los 184 MB de PAK al disco ni edita config.cfg al preparar la imagen. El juego puede guardar los ajustes solicitados al salir.

Quake II (`quake-bulk-final`): demo con GPU, menú principal → Game → partida nueva, salto de la introducción, mapa base1, movimiento, menú y vuelta al juego comprobados mediante capturas y entrada QMP. Sin agotamiento VMA ni errores de fence/EGL del host en esta ejecución. La introducción se vio negra hasta saltarla; no se da por resuelto el vídeo cinematográfico. La primera precarga de datos del CD sigue siendo apreciable. El recorrido con `--runtime-perf` se conserva como diagnóstico, no como benchmark: interfiere con tiempos y puede entrelazar los mensajes serial.

FIFA final (`fifa-explicit-final`): arranque desde el EXE reparado, menú, selección de equipos/lados, presentación de jugadores y partido activo. La copia al cliente descartable prácticamente desaparece de `present_us`; continúa la copia al pixmap que muestra Wine. Las presentaciones por segundo host de los intervalos registrados fueron 5,35 / 8,14 / 11,09 y, más tarde, 10,60 / 9,29. No se demuestra una mejora estable del FPS global respecto de la referencia de 10,04 / 10,35: las escenas y cámaras difieren y hubo cargas/replays. No hay un replay determinista ni certificación de un partido completo. Una muestra adicional dio un contador serial truncado (94 segundos derivados de copia en 20 segundos); se conserva en los datos originales y se marca inválida en benchmark.json. El lector anterior aceptaba una última línea todavía incompleta al leer COM1 mientras QEMU la escribía. Ahora exige la línea completa y rechaza contadores acumulativos decrecientes; la prueba reproduce esa carrera con snapshots parciales.

Las pruebas de partido se hicieron sin audio para comparar con la referencia. Esta ronda no certifica sonido, mandos, todas las cinemáticas ni el rendimiento en hardware físico. Se observó un aviso de shootdown TLB incompleto durante el arranque de las pruebas de Quake, anterior a la última optimización de mprotect y también después; no se ha aislado. Las pruebas explícitas de permisos, fork y contenido de memoria pasaron.

## Imagen preparada y cierre

`build/bleskernos-wine-tuned.img` conserva 6147 archivos idénticos, cambia 9 archivos de runtime/configuración/driver y agrega únicamente el EXE de FIFA, su respaldo, el respaldo del registro y PLAY-GL.CMD. Se verifican modos, respaldo del registro, boot contra el kernel de producción y la estructura BFS4. La imagen original `build/bleskernos-wine-games32.img` se conserva aparte; los SHA-256 están en data-preservation.json y manifest.json.

La ejecución final pasa 17 suites de host, el probe GLX (incluido el cambio explícito de composición a presentación directa, ventanas legacy y pixmaps), DirectDraw/OpenGL/Direct3D 9/11 y Notepad con teclado/portapapeles antes y después de reiniciar Vision. No se registraron fallos de fence/EGL en estas pruebas finales. Los resultados no certifican todos los juegos ni eliminan las limitaciones pendientes.

Para repetir la configuración gráfica medida, desde la carpeta con Makefile:

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-wine-tuned.img --gl --disk-interface ahci --audio none --cdrom media/installers/FIFA2000.iso
```

Para Quake II, montar Quake2.iso y abrir C:\Quake2\PLAY-GL.CMD. El arranque en frío todavía puede esperar por servicios de Wine (RpcSs/winedevice); esta ronda no establece un tiempo de carga estable de 20 segundos.
