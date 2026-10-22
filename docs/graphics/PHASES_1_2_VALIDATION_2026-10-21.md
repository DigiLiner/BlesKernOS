# Fases 1 y 2: validación y entrega

Aprobadas para el entorno VirtIO/VirGL probado. Imagen conjunta:
`build/bleskernos-native-3d.img` (768 MiB).
SHA256: `cc16cb83b8ec2c70f955da28fe7341ef3de102c3fa79884d1aac11a6b4fcb163`.

El instalador verificó el paquete y el núcleo contra los informes aprobados.
Una lectura posterior de la imagen comprobó sus 215 archivos, kernel, stage 2,
VirtIOGPU y Vision. La configuración de inicio se conservó. La imagen base y la
imagen aceptada de fase 1 no se sobrescribieron.

## Entorno y resultados

QEMU 8.2.2 con KVM, un CPU virtual, 768 MiB de RAM, VirtIO VGA GL y SDL.
Anfitrión Celeron N4020 / Intel UHD Graphics 600 (GLK 2).
Mesa 24.3.4 VirGL expuso OpenGL 4.3 de compatibilidad.
Todas las mediciones siguientes usan ventanas de 640 × 480.

Glmark2 2023.01, escenas originales: **192 puntos**.

| Escena | Duración | FPS |
| --- | ---: | ---: |
| build | 10 s | 223 |
| shading / phong | 10 s | 260 |
| jellyfish | 30 s | 105 |
| refract | 45 s | 38 |
| bump / normals | 10 s | 339 |

Cada escena superó el umbral de 30 FPS. Pasaron shaders, FBO, profundidad,
stencil, recursos compartidos, 32 actualizaciones asíncronas de VBO, lectura de
píxeles, orientación de la imagen, cierre y reapertura del contexto. La
presentación nativa registró cero descargas a CPU y scanout GPU directo.

La primera aceptación de fase 1 dio 170 puntos con intervalos distintos
(jellyfish 15 s y refract 30 s). No corresponde convertir la diferencia de
puntaje en un porcentaje de mejora. Las pruebas intermedias mostraron variación
de rendimiento y quedaron conservadas, incluidas las que no alcanzaron el
umbral. La aceptación final corresponde a los intervalos de la tabla.

Mesa demos 9.0.0, escenas originales, 20 segundos por ejecución GPU:

| Demo | Mesa 3.5 / GPU estricta | Gallium / VirGL |
| --- | ---: | ---: |
| gears | 124,72 FPS | 180,18 FPS |
| gloss | 93,87 FPS | 115,04 FPS |
| morph3d | 50,78 FPS | 59,70 FPS |

Los tres demos superaron 30 FPS en ambos backends. Mesa 3.5 registró cero
fallbacks, fallos estrictos y descargas durante esos intervalos. Gloss cargó
sus dos texturas una vez. Las ejecuciones de referencia por software duraron
cinco segundos; no se usan para calcular porcentajes de aceleración frente a
las ejecuciones GPU de veinte segundos.

Las capturas y lecturas explícitas se hicieron fuera del cronómetro. Se
compararon imágenes con el reloj de animación fijo y se comprobó cobertura del
objeto para rechazar imágenes vacías. El error medio sobre el objeto, en niveles
RGB de 0 a 255, fue 2,60 para gears, 1,53 para gloss y 0,46 para morph3d; el límite
fue 8. También pasaron los tres clics de cierre y la recepción del evento por
cada aplicación. El ejemplo nativo existente `LBSTRI.BEX` cargó la biblioteca
actualizada `NGS3D.LBS`, dibujó su triángulo y terminó con `ALL PASS`.

El control con VirtIO sin 3D rechazó limpiamente los contextos Gallium: no hubo
renderer por software oculto ni fallo del kernel.

## Cambios entregados

- Winsys VirGL compartido y cliente gráfico ELF32/POSIX para aplicaciones del
  OS; glmark2 funciona mediante ventanas nativas, independiente de Wine y X11.
- Presentación GPU con tres capturas, fences reales, contrapresión y protección
  de recursos durante el cierre. Mesa 3.5 reutiliza esa cola en VirGL.
- Mesa 3.5 conserva texturas al cambiar el drawable, recupera espacio bajo
  presión de VRAM y publica el color modificado por software antes de exponer
  una superficie GPU. Los fallos de envío impiden publicar imágenes obsoletas.
- Iluminación de ambas caras con selección y restauración del color correcto,
  sin copiar todos los atributos de cada vértice. Se corrigieron la escala de
  polygon offset y un desplazamiento vertical de un píxel.
- Las lecturas explícitas resuelven el contenido sin marcar renderizado por
  software; los temporales grandes de empaquetado usan heap. La compilación
  detecta cambios de cabeceras y argumentos, y optimiza el recorrido de CPU con
  SSE1, sin suposiciones de fast-math.
- Ports de gears, gloss y morph3d, controles nativos, GLU real y teapot original.
  Se reconstruyeron la biblioteca estática y `NGS3D.LBS`. El paquete incluye
  avisos, fuentes GLU y objetos para reenlazar las demos. Se comprobó que el
  reenlace de gears reproduce exactamente el ejecutable entregado.

## Alcance de la aceleración

Mesa 3.5 conserva transformación, luces, clipping y preparación de primitivas
en CPU; rasterizado, texturas, profundidad y mezclas compatibles se envían a
GPU. No incorpora una API GLSL. En Gallium, las operaciones compatibles de la
API fija pueden ejecutarse mediante shaders GPU. La generación procedural de
mallas que realiza cada aplicación sigue siendo trabajo de esa aplicación.
La compilación de shaders se realiza en CPU; los programas compilados se
ejecutan en GPU.

El cliente Gallium usa ELF32/POSIX. Los clientes freestanding LBS existentes
reciben las mejoras de Mesa 3.5 mediante NGS3D.LBS.

SVGA3D conserva Mesa 3.5: su interfaz actual POSITIONT no proporciona el
transporte de contextos, shaders, recursos y fences que exige el winsys Gallium
svga. Ese port requiere trabajo adicional y validación en VMware real.
Los resultados de esta VM no certifican rendimiento de ATI Rage 128 ni GMA
8xx físicas; sus controladores corresponden a la fase siguiente.

## Evidencia y reproducción

- `build/native-gallium/final-validation2/result.json`: ejecución final glmark2.
- `build/native-mesa-demos/final-validation6/result.json`: ejecución de demos,
  imágenes, cierre y ejemplo LBS.
- `build/native-mesa-demos/final-package/result.json`: equivalencia del paquete
  después de añadir fuentes, avisos y objetos de reenlace. Compara los 185
  archivos previamente probados y el núcleo; permite únicamente 30 adiciones
  pasivas. No repite ni altera las mediciones de ejecución.
- `build/native-gallium/final-no-3d/result.json`: control negativo aprobado.
- `build/native-gallium/installed-image-validation.json`: lectura de la imagen
  instalada y comprobación de hashes.
- `build/native-mesa-demos/relink-verification.json`: igualdad del reenlace.

Las doce pruebas de transporte, fences, vida de recursos, estado de Mesa,
publicación y lectura de píxeles pasaron. También compilaron las variantes ELF,
estática y LBS. Los comandos y registros de cada VM están junto a sus informes.

Construcción: `make mesa-demos-native`. La validación de demos se ejecuta con
`make mesa-demos-native-test`. El instalador exige los informes aprobados para
el mismo núcleo y paquete; véanse [Gallium nativo](NATIVE_GALLIUM.md) y
[Mesa demos nativos](NATIVE_MESA_DEMOS.md).
