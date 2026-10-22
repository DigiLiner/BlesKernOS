# Corrección de tirones en la presentación 3D

## Problemas corregidos

- Una captura con el renderizado terminado podía reciclarse antes de que Vision
  la consumiera. El contador de swaps crecía aunque esos cuadros no llegaran al
  compositor. La captura conserva ahora su propiedad hasta el consumo; las
  superficies con lectores GPU pendientes conservan sus fences reales.
- La cola limita las capturas pendientes: una por defecto para presentación
  interactiva, también en Gallium. Glmark2 solicita explícitamente tres mediante
  `BKG_PRESENT_AHEAD=3` y el flag `BLES_GFX_OPEN_PIPELINED`.
  El pool tiene cuatro superficies para dar margen a los lectores GPU del
  escritorio sin reciclar una captura no consumida.
  Una sola en Gallium impedía superponer dibujos GPU con la consulta de fences
  del anfitrión y redujo `refract` a 24 FPS en una ejecución rechazada.
- Vision copia el viewport cuando corresponde presentar el escritorio, en vez
  de copiarlo ante cada aviso del productor. Una captura todavía incompleta
  no consume un período de presentación con un repintado vacío.
- Una invalidación con coordenadas antiguas del reloj reconstruía todo el
  escritorio GPU cada segundo. Se consulta el rectángulo real del panel;
  el panel actual devuelve un rectángulo vacío porque no tiene ese reloj.
- La primera publicación de un viewport solicita su captura de fondo y escena
  completa. La inicialización no depende de una invalidación periódica ni de
  mover el mouse. Se aplica tanto a la cola nativa como a la publicación IPC.
- El sondeo de fences pendientes puede notificar de nuevo la cola VirtIO una
  vez por tick. No espera ni inventa una finalización: conserva la comprobación
  de la respuesta y del fence real. En QEMU 8.2.2, el callback de control llama
  al sondeo del renderer incluso con la cola vacía; el temporizador alternativo
  se programa a 10 ms. Referencias primarias:
  [callback de control](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/hw/display/virtio-gpu-gl.c),
  [sondeo de fences](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/hw/display/virtio-gpu-virgl.c).
- La copia desde la captura al viewport de Vision usa un blit GPU de RGBA con
  su propio fence real. Reemplaza el borrado, el quad texturizado y el sellado
  posterior de una copia de tamaño idéntico, reduciendo trabajo de presentación.
- El lanzador y las pruebas usan GTK por defecto; SDL sigue disponible mediante
  `--display sdl`. Se conservan los resultados de ambas ventanas del anfitrión.

No se cambiaron las escenas originales, su resolución, la calidad de los
shaders ni las rutas de geometría e iluminación de Mesa 3.5.

## Medición y criterios

Se cuentan únicamente envíos exitosos del compositor con contenido GPU nuevo.
Un intento de repaint que espera la escena del shell o termina sin enviar un
cuadro no incrementa esa medida. Los histogramas se almacenan en memoria y se
imprimen al cerrar la ventana, sin imprimir una línea por cuadro.

Son tiempos de envío del escritorio, con resolución de 3,33 ms en esta VM;
no son timestamps de refresco del monitor ni una prueba de VSync. Se incluyen
los intervalos de inicialización y los cambios de escena. El máximo conserva
el índice del intervalo para distinguir picos de apertura de pausas posteriores.

El criterio mínimo adicional exige p95 <= 50 ms, p99 <= 83,334 ms y menos de 1 %
de intervalos mayores de 100 ms. Se conservan los máximos y todos los resultados
intermedios; pasar ese criterio no significa mantener 60 FPS constantes.

El fixture de prueba consulta nombres y estados de procesos sin solicitar un
censo de memoria del heap a 5 Hz. Ese censo no es necesario para comprobar el
cierre de las aplicaciones y agrega trabajo con el lock de memoria tomado.

## Resultados finales

**Estado: candidata para revisión; aprobación global de rendimiento pendiente.**

Las 15 ejecuciones de Mesa demos pasan las comprobaciones funcionales y las seis
ejecuciones con GPU pasan los criterios de cadencia. No hay descargas ni fallback
en los intervalos medidos de la ruta Mesa 3.5 acelerada. La comparación de imágenes
con tiempo fijo, el cierre y reapertura y la prueba nativa LBS también pasan.

| Demo | Backend | Envíos/s | p95 ms | p99 ms | Máximo ms |
|---|---|---:|---:|---:|---:|
| Gears | mesa35 | 55.7 | 26.7 | 36.7 | 103.3 |
| Gears | gallium | 55.9 | 26.7 | 36.7 | 363.3 |
| Gloss | mesa35 | 42.8 | 46.7 | 63.3 | 173.3 |
| Gloss | gallium | 54.1 | 26.7 | 40.0 | 553.3 |
| Morph3D | mesa35 | 57.6 | 23.3 | 36.7 | 136.7 |
| Morph3D | gallium | 50.1 | 33.3 | 43.3 | 486.7 |

Glmark2 pasa shaders, FBO, orientación, cierre y reapertura, pero **falla**
el umbral de rendimiento y los criterios de cadencia. Puntaje: 82.

| Escena | FPS publicados por glmark2 | Tiempo medio ms |
|---|---:|---:|
| build | 98 | 10.221 |
| shading | 109 | 9.220 |
| jellyfish | 78 | 12.974 |
| refract | 28 | 36.765 |
| bump | 102 | 9.852 |

En los 4362 intervalos de glmark2: 40.2 envíos/s,
p95 53.3 ms, p99 93.3 ms y máximo 1333.3 ms
en el intervalo 2590. Hay 25 intervalos mayores de 100 ms.
La medida agrega las cinco escenas; no implica esa cadencia en cada escena.
Glmark2 redondea hacia arriba los FPS publicados; el tiempo medio conserva una
medida más precisa. No se asegura que la escena de refracción alcance 30 FPS.

Una prueba adicional consultó capturas pendientes cada tick, en vez de usar la
espera normal del servicio. No aportó una mejora comprobada: puntaje 73,
p95 56,7 ms y p99 100 ms. **Se retiró**; esos binarios no están en la candidata.
La ejecución de Mesa de ese experimento se interrumpió al descartar el cambio.

Las 18 pruebas de regresión pasan. El control sin aceleración 3D disponible
rechaza la creación del contexto sin inventar un renderer ni bloquear el sistema.
El instalador de versiones aprobadas rechaza los resultados actuales de glmark2
antes de crear una imagen. No se bajó ningún umbral para obtener una aprobación.

### Imagen candidata

`build/bleskernos-native-3d-candidate.img` contiene los mismos binarios y paquete medidos, conserva
el inicio normal y se generó por separado como candidata sin aprobación.
Se verificaron los 215 archivos del manifiesto, el driver, Vision, kernel y stage2.
La imagen original del usuario coincide con la copia inicial y no fue modificada.

La candidata permite revisar las correcciones; **no es una versión aprobada por
rendimiento**. Siguen pendientes las pausas de glmark2 y el margen en `refract`.

SHA-256 de la candidata:

`e6e8d173fbfe3904026e97175582396c2ad41226a1298ee3a9d9306386425170`

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-native-3d-candidate.img \
  --gl --display gtk --memory 768 --smp 1 --audio none
```

Evidencia final:

- `build/graphics-pacing/policy-final-mesa/result.json`
- `build/graphics-pacing/policy-final-glmark/result.json`
- `build/graphics-pacing/policy-no-3d/result.json`
- `build/graphics-pacing/regressions-final.json`
- `build/graphics-pacing/installed-image-validation.json`
- `build/graphics-pacing/candidate-provenance.json`
- `build/graphics-pacing/candidate-smoke.json`
- `build/graphics-pacing/current-installer-rejection.json`

## Reproducción

Ejecutar desde el directorio del proyecto, usando una carpeta de salida nueva:

```sh
python3 tests/graphics/run_native_gallium.py \
  --base build/bleskernos-native-3d.img --mesa-demos --pacing-trace \
  --out build/graphics-pacing/recheck-mesa --seconds 400
```

`--pacing-only` y `--demo-scene` producen diagnósticos que no pueden aprobar
una instalación. El instalador permite exigir los criterios adicionales con
`--require-pacing`; se comprobó que rechaza una aprobación antigua basada solo
en FPS antes de crear una imagen.

Los logs, JSON, capturas y hashes están en `build/graphics-pacing/`. Se eliminaron
discos desechables de diagnósticos terminados para recuperar espacio; se
conservaron los informes y las imágenes originales del usuario.

## Alcance

Validado en QEMU/KVM con VirtIO/VirGL, GTK, audio desactivado, 768 MiB y un CPU virtual, con el
anfitrión Celeron N4020 / Intel UHD Graphics 600. Los resultados no certifican
fluidez en tarjetas físicas ni implementan Gallium para SVGA3D.

Los puntajes antiguos medían otra política de presentación y no son una base
válida para calcular una mejora porcentual con la cola acotada actual. Las
pausas aisladas siguen reflejadas en los máximos: no se afirma fluidez perfecta
ni un refresco físico constante de 60 Hz.
