# Auditoría de aceleración gráfica — 21 de octubre de 2026

Revisión del árbol de trabajo actual, incluidos los cambios locales existentes.
El alcance son los adaptadores PCI x86 reconocidos por los drivers gráficos.
Las puntuaciones describen funciones implementadas y sus condiciones de uso;
no son benchmarks, porcentajes de uso de GPU ni certificaciones de hardware.
En esta revisión no se arrancó una VM ni se probó una tarjeta física.

## Resultado principal

Hay dos integraciones diferentes de Mesa:

1. **Mesa 3.5 del SDK y su variante para Wine:** el pipeline TNL calcula en
   CPU matrices, transformación, iluminación y clipping. El backend recibe
   `SWvertex` con coordenadas de ventana y colores ya calculados; envía
   triángulos a GFX3D. La GPU acelera el trabajo por píxel que soporte el driver.
2. **Mesa 24.3.4/Gallium para Wine/TinyX con VirtIO/VirGL:** un winsys propio
   transporta recursos, shaders y dibujos al renderer del host. Puede ejecutar
   vertex y fragment shaders en la GPU del host. Su presentación actual
   resuelve a RAM y usa `XPutImage`, por lo que conserva readbacks y copias.

La ruta Gallium sólo se construye con `-Dgallium-drivers=virgl`. No habilita
automáticamente Intel, Radeon, NVIDIA ni SVGA3D. No incluye llvmpipe/softpipe;
eso no impide que el renderer OpenGL del **host** sea software. Para atribuir
ejecución a hardware hay que verificar también el renderer del host.

## Inventario y puntuación

Escala **ordinal de 0 a 5**, por separado para 2D y 3D. Cada nivel representa
una clase de integración; no se suman funciones ni se calcula un promedio:

| Nivel | 2D | 3D |
|---|---|---|
| 0 | Framebuffer y dibujo por CPU | Rasterización por CPU |
| 1 | Presentación por regiones y/o cursor del dispositivo | Triángulos con color, Z y mezcla; sin texturas |
| 2 | Blitter nativo de relleno/copia, limitado | Rasterización fija con textura, Z y mezcla |
| 3 | Blitter más superficies/presentación VRAM extendida | Nivel 2 más composición GPU de viewports |
| 4 | Composición, escalado y transformaciones por GPU, contenido de UI aún generado en CPU | Pipeline programable, presentación todavía con readback |
| 5 | Escritorio general residente en GPU, incluidas rutas habituales de dibujo | Pipeline programable y presentación residente sin readback habitual |

El nivel se aplica a la ruta indicada cuando supera sus probes/autopruebas.
Si no se habilita, hay que puntuar la ruta de fallback efectivamente usada.
Ninguna ruta revisada alcanza el nivel 5 bajo estos criterios.

| Adaptador/ruta | 2D | 3D | Trabajo acelerado implementado | Condiciones y límites |
|---|---:|---:|---|---|
| VMware SVGA II `15ad:0405`, sin backend 3D operativo | 1 | 0 | Cursor, notificación de regiones modificadas | Fill/copy legacy por CPU en build predeterminada |
| VMware con `VMWARESVGA3D.DVR` operativo | 4* | 3 | Rasterización, textura, Z, alpha test, mipmaps, blending; composición/escalado de viewports | TNL CPU; protocolo 3D legacy; sin Gallium/GLSL propio |
| VirtIO `1af4:1050` / `1af4:1010`, sin VirGL | 1 | 0 | Transferencia/presentación por regiones, cursor | Fill/copy y 3D por CPU; `1010` requiere transporte PCI moderno |
| VirtIO/VirGL con Mesa 3.5/TinyGL nativo | 4* | 3 | Rasterización, texturas, Z, alpha test, mipmaps y blending; composición y scanout 3D | Transformación/luces CPU; shaders internos fijos, sin GLSL de aplicación |
| VirtIO/VirGL con Gallium 24.3.4 en Wine | 4* | 4 | Vertex/fragment shaders, texturas, FBO y estados admitidos por capsets | El 2D corresponde al driver nativo; Wine descarga color para TinyX |
| ATI Rage Mobility M3/LF `1002:4c46` | 3 | 2 | Fill, copias/ROP, superficies VRAM, cursor y overlay; rasterización texturada, Z16, mezcla, alpha test, mipmaps | Sin composición GPU de ventanas; Z16 experimental y validación física de la revisión pendiente |
| Intel 852/855 `8086:3582` | 2 | 1 | XY blitter; triángulos con color interpolado, Z16 y alpha blend | 3D sólo a 32 bpp y tras autoprueba; sin texturas ni shaders; físico pendiente |
| Intel 915/945: `2582`, `2592`, `2772`, `27a2`, `27ae` | 2 | 0 | XY blitter de relleno/copia | No se registra backend 3D para estos IDs |
| Intel `2972`, `2982`, `2992`, `29a2`, `29b2`, `29c2`, `29d2`, `2a02`, `2a12` | 2 | 0 | XY blitter de relleno/copia | No se registra backend 3D; el grupo interno «generation=4» no basta para elegir un driver Mesa |
| ATI Rage/Radeon reconocidas sólo por LEGACYFB | 0 | 0 | Scanout del framebuffer configurado por firmware | Reconocimiento PCI y aperture VBE válidos; sin motor ATI/Radeon |
| NVIDIA RIVA/GeForce/Quadro reconocidas por LEGACYFB | 0 | 0 | Igual que arriba | Sin nouveau ni driver de comandos NVIDIA |
| Cirrus GD54xx, Trident TGUI/Cyber/Blade, Matrox Millennium/G, SiS, 3dfx Banshee/Voodoo y S3 Trio/ViRGE/Savage de LEGACYFB | 0 | 0 | Igual que arriba | Sin motores 2D/3D específicos; las capacidades físicas varían por chip |
| VGA/VESA genérico | 0 | 0 | Salida de imagen | Mesa puede renderizar en CPU |

\* Nivel 4 disponible para la **composición de viewports GPU**. No significa
que cada ventana o widget del escritorio se dibuje con GPU. El compositor
general sigue usando superficies pintadas por CPU; `gui_gpu_compositor_needed`
activa la ruta adicional cuando existe un viewport GPU visible.

El [inventario CSV](ACCELERATION_PCI_2026-10-21.csv) enumera individualmente
los IDs explícitos de las tablas, incluyendo la preferencia del driver ATI
nativo frente a LEGACYFB. Estar en esa lista significa reconocimiento en el
código, condicionado al dispositivo/modo/aperture; no prueba funcionalidad en
cada placa comercial. VESA genérico no se limita a esa lista PCI.

## ¿La geometría y los shaders los hace la GPU o la CPU?

«Geometría» incluye varias etapas. Construir una malla en la aplicación,
transformar sus vértices y rasterizar sus triángulos son tareas distintas.

| Etapa | Mesa 3.5 → GFX3D | Gallium → VirGL en Wine |
|---|---|---|
| Lógica de escena y generación de mallas | CPU de la aplicación | CPU de la aplicación, salvo algoritmos explícitos implementados con funciones GPU admitidas |
| Matrices, transformación y luces clásicas | CPU/TNL | Pueden traducirse a shaders y ejecutarse en GPU del host |
| Vertex shader de aplicación | API GLSL no disponible | GPU del host para los programas admitidos |
| Clipping y preparación | TNL CPU; scissor de primitivas también recortado por backend CPU | Pipeline GPU para el dibujo admitido; preparación de comandos en CPU |
| Quads, líneas y puntos | Conversión/expansión a triángulos en CPU | Según primitivas y capacidades del driver |
| Cobertura e interpolación del triángulo | GPU si el estado y driver son compatibles; CPU/SWRAST en fallback | GPU del host |
| Muestreo/filtrado de textura | GPU donde hay `TEXTURES`; Intel Gen2 vuelve a software | GPU del host |
| Z, alpha test y mezcla | GPU según capacidades; restricciones del backend común | GPU según formato/estado/capset |
| Fog lineal | Factor y mezcla de color por vértice en CPU; interpolación GPU | Puede implementarse por shaders |
| Fragment shader de aplicación | No hay GLSL; operaciones fijas del backend | GPU del host para programas admitidos |
| Compilación de shaders | No hay compilador GLSL en Mesa 3.5 | CPU del guest y del host; ejecución del programa en GPU |
| Geometry shader, teselación, compute | Sin API/ruta programable | No certificados por esta auditoría; consultar versión/extensiones/capsets y probar cada función |
| Presentación | Nativo puede evitar readback con viewport en VMware/VirGL; ATI puede necesitarlo | Readback de color y envío a TinyX |

En VirtIO nativo **sí hay shaders internos**: un VS TGSI de paso y FS de
color/textura/DECAL. El VS copia las entradas; el driver reconstruye coordenadas
clip desde los vértices de pantalla. Que haya un shader en GPU no significa
que las matrices o las luces de la aplicación hayan salido de la CPU.

En VMware se envía `POSITIONT`, es decir, posición ya transformada.
`GFX3D_CAP_TRANSFORM` describe transformaciones de composición de superficies;
no demuestra T&L de las mallas en hardware. En Intel Gen2 el estado desactiva
viewport transform y perspective divide; recibe XYZ/color preparados.

## Qué limita hoy Mesa 3.5

El código actual admite una unidad de textura 2D, MODULATE/REPLACE y DECAL
cuando el driver lo anuncia; alpha test condicionado a capacidad; filtros
nearest/linear y mip-nearest. Con `TEXTURE_MIPMAPS` sube niveles nativos; sin
esa capacidad selecciona la imagen mediante la estimación CPU por triángulo.
No hay trilineal en el subconjunto acelerado.

Stencil activo, multitextura, texturas 1D/3D/cubo, coordenada proyectiva q≠1,
logic ops, máscaras parciales de color, varios estados de smoothing/stipple,
fog EXP/EXP2 y otros estados quedan fuera de esa ruta. SWRAST mantiene
compatibilidad en AUTO/GPU; GPU_STRICT registra el fallo. Un cambio GPU↔CPU
puede exigir sincronizar color y Z16, de modo que un estado pequeño puede
tener un costo grande de transferencia y espera.

Hay caché de 64 entradas de textura/imagen y lotes de hasta 3072 vértices.
VirtIO anuncia `ORDERED_SUBMIT`; VMware y ATI no lo anuncian. El driver ATI
drena CCE/caché por draw público y divide internamente en paquetes de 384
vértices. Intel Gen2 sincroniza cada bloque de hasta 96 vértices y limpia Z
con un bucle CPU: incluso un dibujo sin fallbacks conserva costos CPU/esperas.

**Documentación anterior desactualizada:** `mesa35.md` todavía enumera alpha
test como no acelerado y describe sólo la selección manual de mip; el código
ya contempla alpha test, DECAL, mipmaps nativos y blend multiplicativo en
VirtIO. `INTEL_GMA9XX.md` omite el backend Gen2 existente.

## Potencial del hardware y trabajo necesario

Mesa necesita un driver/winsys conectado al kernel; instalar una versión más
nueva no habilita por sí solo motores que sólo usamos como framebuffer.

| Familia | Qué permitiría aprovechar mejor | Trabajo en BlesKernOS |
|---|---|---|
| VirtIO/VirGL | Pipeline programable que ya funciona por Gallium en Wine | Llevar esa integración al SDK nativo y compartir/publicar superficies para evitar readback en Wine |
| VMware | Mesa upstream tiene driver Gallium `svga` programable | Implementar winsys/recursos/comandos apropiados; la ABI legacy actual no equivale al soporte moderno de upstream |
| Rage128 M3 | Más operaciones del pipeline fijo y composición mediante texturas | Implementar `surface_composite`, integración de viewport y medir PIO/estado/esperas; no tiene sentido prometer GLSL moderno cambiando Mesa |
| Intel 852/855 | Texturas y más estados del pipeline fijo | Extender y probar el backend i830/Gen2; priorizar textura y presentación antes de prometer geometría GPU |
| Intel 915/945, Q35/G33/Q33 | Rasterización y fragment shaders con el driver Mesa i915 | Portar memoria, buffers, relocaciones, envío y sincronización/winsys. El i915 de Mesa usa `draw` para vertex shaders CPU: un port completo tampoco elimina todo el trabajo geométrico CPU |
| Intel 946/G35/Q965/G965/GM965/GME965 de los IDs revisados | Pipeline programable mediante la familia Mesa crocus | Backend del kernel y winsys distintos; validar por ID y hardware, sin copiar el tratamiento de Q35/G33/Q33 |
| Radeon y NVIDIA sólo VBE | Algunas generaciones tienen T&L/shaders; otras son anteriores | Elegir chips concretos y portar sus drivers/memoria/comandos. La etiqueta comercial de familia no define un nivel de shaders común |
| Otras familias LEGACYFB | Muchas tienen blitters 2D; algunas también 3D fijo | Driver por chip/generación. No asumir que toda una familia tiene 3D o shaders |

La diferencia Intel se verificó contra el código fuente upstream **24.3.4**
disponible en `build/wine/gallium/mesa-24.3.4`: Q35/G33/Q33 están en
`include/pci_ids/i915_pci_ids.h`; los otros seis IDs del último grupo Intel
están en `crocus_pci_ids.h`. `i915_state.c` crea los vertex shaders con `draw`.
Esto es una referencia para futuros ports, no drivers hoy empaquetados.

## Mejoras en orden propuesto

1. **Medir la ruta efectiva por aplicación y estado.** Registrar backend Mesa,
   driver, caps, fallbacks por causa, tiempo CPU/TNL, tiempo de envío/espera,
   readback, bytes transferidos, composición y percentiles de tiempo de frame.
   Separar trabajo CPU de espera de GPU; un contador de fences no mide su costo.
2. **ATI: medir y eliminar el viaje de presentación a RAM.** La falta de
   `surface_composite` impide el viewport residente. Antes de cambiar PIO o
   quitar esperas, usar los scopes existentes de estado/PIO/lock/readback.
   Hay un reporte histórico de ~2 FPS en el cubo; aún no prueba cuál costo domina.
3. **Wine/VirGL: presentación de superficies compartidas.** El shader ya puede
   ejecutarse en GPU, pero `glx.c` resuelve y hace `XPutImage`. Requiere importar
   recursos entre los contextos aislados y preservar propiedad/fences; no basta
   con pasar un handle privado al compositor.
4. **Aplicaciones nativas: acceso al pipeline Gallium de VirGL.** Es la vía con
   mayor avance existente para pasar matrices/luces/shaders al host. Requiere
   adaptar runtime/SDK o ampliar GFX3D con buffers, programas y uniforms.
   No se logra sólo reemplazando el callback de triángulos post-TNL.
5. **Intel Gen2: validar físico y añadir texturas.** Optimizar después el clear
   de Z, batching y sincronización con pruebas de coherencia; conservar la
   exclusión compartida 2D/3D y el rechazo de capacidades no implementadas.
6. **Intel posteriores: separar rutas i915 y crocus por PCI ID.** Es una
   ampliación grande de driver, no una activación del 3D Gen2 existente.
7. **2D general: conectar primitivas/presentación a superficies GPU.** Hoy
   disponer de un blitter no evita que los widgets pinten en RAM. En VMware
   evaluar composición 3D antes de activar globalmente comandos legacy que
   están deshabilitados por incompatibilidades documentadas.

## Cómo comparar rendimiento real

Usar por tarjeta la misma resolución, profundidad, CPU, escena y compilación;
separar nativo/Wine y GPU_STRICT/AUTO. Medir calentamiento y luego un intervalo
estable con mediana, p95/p99 de frame y uso CPU, junto con imagen correcta.
En virtualización registrar KVM/TCG, GPU/renderer del host y configuración 3D.

Escenas separadas: fill/copia/scroll 2D; composición/escalado; muchos vértices
con pocos píxeles; muchos píxeles con pocos triángulos; textura/mips;
alpha/depth; shader programable; alternancia de estados que causan fallback;
presentación con y sin readback. Comparar también una referencia CPU.

Para clientes Mesa 3.5, `bk_mesa_get_gpu_stats` y GPU_STRICT permiten verificar
draws reales, fallbacks, cargas y descargas. `fallbacks=0` no significa TNL GPU
ni ausencia de espera o copia de presentación. Para Gallium verificar renderer,
versión/extensiones, resultados de shaders/FBO y tiempos de resolve/present.
Los contadores propios de Mesa 3.5 no cubren automáticamente Gallium.

La evidencia anterior de VirtIO describe ~21–23 FPS de carrera tras corregir
el VBO; invalida mediciones previas con corrupción. Es historial de otra
ejecución/build, no un benchmark nuevo ni comparable con el cubo ATI.

## Evidencia y validación de esta revisión

Fuentes principales del árbol:

- `libs/Mesa-3.5/src/BlesKernOS/bkmesa_gpu.c`: `state_supported`,
  `convert_vertex`, `submit_triangle`, `install_driver` y sincronización híbrida.
- `kernel/include/gfx3d_driver.h`: contrato de triángulos y callback VirGL.
- `kernel/drivers/graphics/{vmware_svga_dvr.c,vmware_svga3d.c}`:
  gate experimental 2D, POSITIONT, capacidades y composición.
- `kernel/drivers/graphics/{virtio_gpu_dvr.c,virtio_gpu_gallium.inc}`:
  shaders internos, anillo de vértices y transporte Gallium.
- `kernel/drivers/graphics/{ati_rage128_dvr.c,ati_rage128_3d_dvr.c}`:
  ID concreto, motor 2D, capacidades 3D y límites de presentación.
- `kernel/drivers/graphics/{intel_gma9xx_dvr.c,intel_i830_3d.inc}`:
  IDs, blitter, gating Gen2 y envíos síncronos.
- `kernel/drivers/graphics/legacy_fb.c` y `families/*.inc`: identidad VBE.
- `gui/gpu_compositor.c`, `gui/compositor.c`, `gui/gfx.c`: activación y copias.
- `ports/wine/graphics/{build-gallium.py,virgl-winsys.c,glx.c}`:
  versión fijada, único driver VirGL y presentación por XPutImage.
- [Validación VirtIO/ATI](VIRTIO_R128_VALIDATION.md),
  [estado físico ATI](ati-rage128-3d.md),
  [auditoría ATI](ATI_R128_PERFORMANCE_AUDIT.md),
  [validación VMware](SVGA3D_VALIDATION.md) y
  [evidencia Intel](../../tests/kernel/evidence/travelmate250-2026-10-15/README.md).

Pruebas host ejecutadas en esta revisión:

- `python3 tests/desktop/test_mesa_gpu_state.py`: PASS, traducción de color,
  paletas, REPLACE/MODULATE y gating de blend multiplicativo.
- `python3 tests/kernel/test_intel_gen2.py`: PASS en host e i386 con
  ASan/UBSan, límites, arena, pitches, paquetes y rechazo de entradas.

Estas pruebas verifican software; no certifican rasterización física ni FPS.

Referencias primarias externas consultadas:

- [Mesa: VirGL](https://docs.mesa3d.org/drivers/virgl.html): arquitectura
  guest/host, transporte de shaders y aceleración a través del host.
- [Mesa: VMware SVGA3D](https://docs.mesa3d.org/drivers/svga3d.html): el driver
  upstream y sus requisitos son distintos del backend legacy de este OS.
- [Mesa: Amber](https://docs.mesa3d.org/amber.html): separación entre drivers
  clásicos y sucesores Gallium; portar Mesa requiere elegir la ruta adecuada.
