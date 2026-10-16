# Auditoría ATI Rage 128 tras la prueba física de la Dell

## Actualización: correcciones sin una nueva prueba física

A pedido del usuario se avanzó con cambios de código, sin condicionar el trabajo
al protocolo manual de abajo. Los hallazgos de la auditoría describen la ruta
anterior; ahora se aplicaron además:

- `OSMesaBleskResolveColor` y `bkmesa_gpu_resolve_color`: presentación del SDK y
  `OSMesaGetColorBuffer` descargan sólo color. El flag de profundidad modificada
  permanece pendiente. `OSMesaGetDepthBuffer` sincroniza explícitamente antes
  de devolver el puntero CPU; la transición a rasterizado CPU y la resolución
  completa conservan la sincronización color/Z. Se mantiene `glFinish` al
  presentar en RAM. Es un cambio común a todos los backends, no específico de Tux.
- ATI limpia Z16 mediante escrituras de 32 bits con dos valores idénticos por
  escritura; una anchura impar conserva una escritura final de 16 bits. No se
  toca el padding ni se modifica el formato de profundidad.
- Los draws sin textura dejan de escribir los once offsets mip. La textura está
  deshabilitada y cada draw texturado vuelve a programarlos todos, de modo que
  no depende del estado dejado por otro propietario.

Para un frame de 640×480 que antes resolvía color y Z16, presentar sólo color
elimina 614.400 bytes de lectura VRAM y la conversión Z16→Mesa por presentación.
Es una reducción deducida del código, **no una mejora de FPS medida**. La copia
de color y la composición CPU siguen existiendo. No se eliminaron las barreras
que impiden que 2D detenga un draw ATI pendiente.

Validación: `tests/graphics/test_ati_color_present.py` compila funciones C reales con
ASan/UBSan y sustitutos del hardware: resolución de color sin consumir Z,
resolución completa posterior, acceso OSMesa explícito a Z, propagación de error
y 635 clears Z16 con comprobación de padding y guardas. Mesa/SDK, ATI y las
imágenes se reconstruyeron; logs y originales en `build/ati-code-fixes/`.
No se atribuye a estos cambios la desaparición de las líneas de colores sin
poder observar la tarjeta.

## Evidencia y alcance

El usuario informó que la Dell arranca, Tux muestra menú y carrera con geometría,
HUD y texturas, pero la UI está trabada, hay líneas de colores distorsionadas y
el juego es injugable. 3D Plus 1.0 informa Mesa 3.5, `ati_rage128_3d`, «sin
fallbacks», un objeto de 8 vértices/12 caras, cero texturas y aproximadamente
2 FPS. Es evidencia física de funcionalidad y de un problema importante;
no es una medición por etapa ni una validación de los cambios de esta auditoría.
No se dispuso de la Dell, ni se ejecutó una emulación como sustituto.

El alcance del driver sigue siendo Mobility M3/LF 1002:4C46, 32 bpp. No atribuir
los 2 FPS a la antigüedad del chip. Tampoco interpretar las 12 caras del modelo
como el número exacto de primitivas enviadas: 3D Plus añade rejilla, ejes,
selección y, según ajustes, sombras/pasadas. Los nuevos contadores de vértices
y draws permiten comprobar esa diferencia.

## Hallazgos comprobables en el código

| Etapa | Ruta ATI y consecuencia |
| --- | --- |
| CPU/Mesa | Mesa transforma/prepara vértices y agrupa hasta 3072 por lote. La caché de texturas persiste; no se crea el render target en cada frame salvo cambios de contexto/tamaño. «Sin fallbacks» no mide trabajo CPU de preparación/presentación. |
| Clear | `r1283d_begin_render` limpia color y Z16 con bucles CPU sobre VRAM, bajo propiedad del motor. El coste crece con píxeles aunque no haya geometría. |
| Estado/envío | Cada draw reenvía estado completo, incluso once offsets de mip sin textura, invalida caché de texturas y emite flush GUI. El modo es PIO, sin anillo DMA/AGP. Antes se consultaba el FIFO por cada par de DWORDs y se hacía malloc/free por paquete. |
| Espera | Cada draw público drena CCE y caché antes de soltar el motor. `end`, transferencias y cambios de propietario pueden volver a comprobar idle. Las esperas tienen hasta 3 millones de iteraciones; no son un plazo fijo en microsegundos. |
| Fence | El draw devuelve un fence ya completado. `wait_fence` retorna rápido si ya se completó; contar llamadas de fence no equivale a contar esperas reales. ATI no anuncia `ORDERED_SUBMIT`. |
| Readback | ATI no implementa `surface_composite` ni anuncia superficies de ventana. `plus_render.c` entra en `plus_renderer_present_cpu` → `bk_mesa_present` → `glFinish` y `OSMesaBleskResolve`. `bkmesa_gpu_resolve` resuelve color **y profundidad** si están modificados. |
| Trabajo posterior | La rama CPU de 3D Plus ejecuta `plus_postprocess_range`. La conversión Z16→profundidad Mesa y el procesamiento de imagen ocurren en RAM, fuera del tiempo de copia del driver. |
| Presentación/UI | El compositor trabaja en RAM. ATI 2D sube regiones dirty a staging VRAM y hace BitBlt al scanout, o copia RAM→scanout en su ruta alternativa. La presentación directa VRAM→VRAM de GFX3D existe, pero no sustituye esta composición de ventanas. |
| Propiedad | Readbacks, clears y draws comparten el mutex del motor con 2D. Una operación 3D larga puede retrasar la UI. El tiempo del mutex y los cambios de propietario se registran ahora por intervalo. |

Ejemplo de escala, **no medición de la Dell**: a 640×480, leer color cuesta
1.228.800 bytes y leer Z16 otros 614.400. Limpiar ambos y volver a subir sólo
el color suma unos 4,69 MiB de tráfico CPU↔VRAM por frame, sin contar copias
en RAM ni regiones extra de escritorio. El tamaño real debe salir de los
contadores; esta cadena puede dominar una escena de muy pocos triángulos.

Los sospechosos principales son por tanto las transferencias/clears y las
esperas/cambios de contexto. Sin los tiempos físicos no se puede ordenar con
certeza cuánto pesa cada uno. El reenvío de estado/PIO puede afectar mucho a
Tux con muchos draws, pero no demuestra por sí solo el origen de los 2 FPS.

## Comparación con VMware/VirtIO y corrupción visual

VMware/VirtIO disponen de publicación/composición GPU que puede evitar el
readback de cada ventana. ATI mantiene una cadena híbrida GPU→CPU→GPU.
Copiarles únicamente la política de fences no le aporta esa arquitectura.

El fallo de VirtIO consistía en reutilizar almacenamiento de vértices todavía
leído por GPU. En ATI los datos CPU se copian por PIO: no queda una referencia
DMA a ese paquete. Sí importa la propiedad del motor: el puente 2D detiene PM4
al entrar. Se conserva el drenaje de cada draw, el mutex común y la espera al
retirar superficies. La implementación histórica de Linux también exige
terminar comandos antes de detener CCE:
[Linux r128_cce.c](https://raw.githubusercontent.com/torvalds/linux/v2.6.32/drivers/gpu/drm/r128/r128_cce.c).

Las líneas de colores siguen **sin causa demostrada**. Se revisaron las reservas:
la arena 3D empieza después de scanout/staging y deja 64 KiB superiores;
los buffers de color/Z se asignan por páginas. Hay un riesgo separado si se
usa overlay de vídeo concurrente: su asignador superior no consulta las páginas
3D y podría solaparlas. No explica automáticamente un cubo sin vídeo; no se
cambió el asignador ni el modo de memoria sin una reproducción. En la prueba
base no abrir reproductores; indicar si las líneas afectan al viewport, a la
rejilla/ejes, o también al escritorio y si persisten tras cerrar 3D.

## Cambios conservadores de esta revisión

- El envío reserva bloques pares de hasta 192 DWORDs, dentro de la capacidad
  ya usada por el driver, conserva el orden EVEN/ODD y padding PACKET2. No se
  conserva una reserva entre llamadas ni entre propietarios. El límite y la
  respuesta ante timeout permanecen. La GPU sólo puede liberar más espacio
  mientras el único propietario escribe el bloque reservado.
- Paquete CPU de vértices fijo de 10.768 bytes en lugar de malloc/free por
  fragmento. Lo protege la propiedad del motor y se copia completamente por
  PIO antes de reutilizarse. No se cambió la vida de superficies GPU.
- Upload/readback rechazan un pitch que no cubra el rectángulo solicitado;
  upload regional comprueba límites mediante resta para evitar overflow.
  Esto cierra entradas inválidas; **no demuestra que causaran las líneas**.
- Instrumentación opcional común a ATI 2D/3D, sin logs por draw ni por poll.
  La ABI pública `perfmon_snapshot_t` conserva su tamaño; el kernel exporta
  `perfmon_scope_note` para los nuevos DVR. Instalar kernel y drivers juntos.

No se retiraron clears, sincronización de profundidad, flushes de caché ni el
estado por draw: cambiar esos contratos requiere medir y validar visibilidad
primero. Si domina `depth_down`, el siguiente paso justificable sería una API
explícita de presentación sólo-color, manteniendo resolución de profundidad
para los consumidores CPU. Si domina readback de color, estudiar superficies
de ventana/composición ATI es una tarea posterior, no una bandera que activar.

## Prueba física siguiente

Usar `build/bleskernos-usb-user.img` reconstruida con esta revisión. El disco ATA
normal es `build/bleskernos-ata-user.img`. Conservar el disco anterior para comparar;
los artefactos y hashes de esta revisión están en `build/ati-performance-audit/`.

1. Guardar COM1 desde el arranque, incluidos ID PCI, modo, arena/reserva2D,
   `[R128:TEST]`, errores y timeouts. Conservar resolución y ajustes de la
   prueba anterior. No confundir un PASS de arranque con rendimiento validado.
2. En terminal ejecutar `perfmon interval 10`, luego `perfmon watch`.
3. Capturar al menos dos intervalos completos por caso: escritorio quieto;
   mover una ventana sin 3D; cubo girando; mismo cubo con viewport menor;
   Tux en carrera; cerrar 3D y volver a mover ventanas. Anotar separadamente
   las horas/fases para no mezclar apertura, carga de texturas y carrera.
4. Si el editor lo permite, repetir el cubo sin rejilla/ejes/sombras y con
   la misma calidad. Anotar FPS visibles y dimensiones del viewport, además
   de la resolución del escritorio. No necesita una escena más compleja.
5. Ejecutar `perfmon off` y repetir brevemente el caso para estimar el coste
   del propio perfilador. Devolver log completo y cuándo aparecen las líneas.

### Lectura de los datos

Cada `[PERF:ATI]` contiene `calls` cronometradas, `events` contabilizados,
`total_us`, `max_us`, `units`, lecturas MMIO `polls` y `timeouts` del intervalo.
Se reinician con los demás contadores de perfmon. No dependen de que una
aplicación llame a la presentación directa ATI, a diferencia del antiguo
reporte acumulado `ATIR1283D:PERF`.

| Etiqueta | Qué mide / unidad |
| --- | --- |
| `lock` | Tiempo de adquisición del mutex compartido; incluye adquisición sin contención. |
| `switch_2d`, `switch_3d` | `events`: cambios de propietario. `switch_2d` también cronometra restauración del contexto, incluidas sus esperas. |
| `start` | Rearranques CCE completados en `events`; tiempo de intentos reales, incluidas esperas. El microcódigo se reutiliza mientras no haya reset. |
| `state` | Programación de estado por draw, inclusiva de submit/PIO. |
| `encode` | Preparación CPU de paquetes; `units` son vértices realmente enviados, con repeticiones de índices/pasadas. |
| `submit` | Emisión de palabras, inclusiva de PIO; `units` son DWORDs de entrada enviados (padding añadido por el serializador no cuenta). |
| `pio` | Espera de espacio PM4, lecturas de estado y timeouts. |
| `idle`, `cache` | Polling desde 3D: espera de motor y espera de caché separadas. |
| `clear` | Escrituras CPU a color/Z; `units` son bytes escritos. |
| `upload`, `download` | Copias color/texturas CPU↔VRAM; incluye regiones y uploads mip. Bytes, sin la espera previa. |
| `depth_up`, `depth_down` | Copias Z16 CPU↔VRAM en bytes, sin la espera previa ni conversión Mesa. |
| `present_2d` | Presentación completa, inclusiva de lock/copias/envío; `units` son rectángulos solicitados. |
| `copy_2d` | Copias DWORD CPU→VRAM, bytes. En la prueba 32 bpp incluye staging y ruta directa. |
| `fifo_2d`, `idle_2d`, `cache_2d` | Esperas 2D desglosadas; `idle_2d` excluye FIFO y caché. |

Complementar con `[PERF:3D]` para draw/readback/end/publicación completos y
`[PERF:TIME]`/`[PERF:GUI]` para composición, porcentaje dirty y actividad del
escritorio. `[PERF:APP]` cuenta publicaciones aceptadas, no scanouts; si el
cliente no produce estas filas, usar los FPS del viewport como aproximación.
`[PERF:PC]` identifica dónde se consume CPU (Mesa, conversión Z, postproceso,
compositor). Para simbolizar conservar los ELF exactos y sus bases de carga;
`tools/symbolize_graphics_perf.py --help` describe los argumentos.

Ejemplo: `download total_us=4000000 units=24576000` durante diez segundos
significa aproximadamente 40% del intervalo en esa copia y 6,14 MB/s mientras
copia. Dividir `total_us` entre frames del mismo caso da microsegundos/frame.
Para composición, `compose=20%` equivale aproximadamente a dos segundos de
ese intervalo. Estas cifras son orientativas, no porcentajes sumables.

**Límites de interpretación:** tiempos de pared por TSC, incluyen desalojo y
pueden cruzar el límite de un intervalo; contadores aproximados en SMP. Hay
scopes anidados: no sumar draw+state+submit+pio ni present+copy+lock. Sin TSC,
los tiempos son cero pero `events`, bytes y polls siguen disponibles. `idle`
es espera observada por CPU, **no tiempo puro de ejecución GPU**: GPU también
trabaja mientras se envían comandos. CPU/Mesa se estima con muestras y tiempos
externos, no restando sin más todos los porcentajes de un frame.

## Validación realizada aquí

- Compilados `ATIR128.DVR`, `ATIR1283D.DVR` y kernel, sin nuevos avisos en los
  archivos modificados. El enlace conserva el aviso previo de GNU-stack;
  la compilación completa también mostró avisos de símbolos sin uso de Wine.
- `python3 tests/graphics/test_ati_pio_submission.py`: prueba el serializador C real
  con FIFO simulado, 3000 longitudes, orden/padding exactos, reservas limitadas
  y fallo de espera sin escrituras posteriores; compilado con ASan/UBSan.
- Reconstruidas imágenes normales ATA/USB y comprobado su contenido.

Esto valida construcción y lógica del envío, **no el hardware, una mejora de
FPS ni la desaparición de las líneas**. La comparación física queda pendiente.
