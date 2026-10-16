# Rendimiento en Latitude C600 y equipos UHCI

Cambios del 10 de octubre de 2026, motivados por pruebas físicas con 128 MiB,
ATI Rage 128 y arranque USB. El audio reportado funciona; no se modificó su ruta.

## USB

El almacenamiento UHCI usaba el buffer de control de 1 KiB para cada tanda bulk.
Cada tanda se publica en el controlador, espera su finalización y espera el
avance de dos frames antes de reutilizar los descriptores. En workers, esas
esperas además ceden tiempo al planificador. Leer 124 KiB necesitaba 124 tandas.

Ahora cada controlador tiene un buffer bulk DMA propio de 8 KiB, alineado y en
memoria física baja, usando hasta los 128 TD existentes. Una lectura de 124 KiB
necesita 16 tandas con paquetes de 64 bytes. Los endpoints de menor tamaño
conservan el límite de 128 paquetes; control continúa con su arena de 1 KiB.
El polling conserva un puntero al primer TD pendiente para evitar volver a
recorrer los descriptores ya completados en cada iteración. El bloqueo,
toggles, paquetes cortos, tiempos máximos y retiro seguro de TD
se mantienen. La reserva estática adicional es 32 KiB para cuatro controladores.
Esto reduce el costo entre tandas; no aumenta la velocidad física de USB 1.1.

## Pantalla ATI Mobility M3

La firma M3 de la ROM identifica un encabezado con punteros, no el registro
que contiene las dimensiones del panel. El driver ahora sigue ese puntero,
valida límites y dimensiones y prioriza ese dato sobre los registros de un
modo de arranque posiblemente escalado. Conserva el fallback por registros y
por modo de firmware cuando la ROM no ofrece información válida.

También obtiene la referencia del PLL de la ROM y calcula el reloj guardado
con los divisores efectivos. Un modo escalado de 800x600 puede conservar el
reloj del panel; asumir siempre el reloj VGA estándar hacía incorrecta la
proporción al cambiar de modo. ROM inválida conserva la ruta anterior.
No se agregaron resoluciones arbitrarias: continúan los modos estándar
conocidos, limitados por panel, memoria de video y validación transaccional.

La estructura de ROM se contrastó con la implementación primaria de
[X11Libre xf86-video-r128](https://raw.githubusercontent.com/X11Libre/xf86-video-r128/master/src/r128_driver.c),
funciones R128GetPanelInfoFromBIOS y R128GetPLLParameters. La integración es
propia; no se incorporó código de terceros.

## Cambio posterior: cascada en 1400x1050 y volumen inicial

Después de comprobar físicamente la detección de 1400x1050, se reportó una
imagen en cascada al seleccionar ese modo. Se encontró que el modeset conservaba
selecciones de reloj dividido y sincronización shadow de `FP_GEN_CNTL` del BIOS.
Ahora el panel usa CRTC1 sin esas selecciones, conserva los bits de alimentación
y retroiluminación y programa sus dimensiones nativas en el escalador. Los
modos progresivos limpian doble barrido, entrelazado y sincronismo compuesto.
En resolución nativa el escalador queda desactivado; en modos menores continúa
activo. También se corrige la compensación horizontal del panel en 8 bpp.
El estado nuevo de FP se guarda y restaura si falla la transacción de modeset.
Se contrastó la programación con `R128InitFPRegisters`, `R128InitLVDSRegisters`
y `R128InitRMXRegisters` del controlador de referencia enlazado arriba.

El volumen predeterminado global pasa a 30%, aplicado antes del sonido inicial.
Las preferencias futuras guardadas siguen teniendo prioridad. La actualización
de estas imágenes usa `--master-volume 30` para cambiar también la preferencia
existente, sin alterar la selección de sonidos ni los eventos habilitados.

`python3 tests/kernel/test_sxga_volume.py` comprueba las funciones de producción
con ASan/UBSan: pitch 8/32 bpp, 1400x1050 sin escalado, modos menores escalados,
limpieza de selecciones FP/CRTC, conservación de alimentación y ganancia PCM.
Estas pruebas no simulan el panel ATI: falta confirmar físicamente que desaparece
la cascada. No se ha demostrado que la causa fuera la frecuencia de refresco;
se conserva el modeline de aproximadamente 60 Hz.

## Administrador de procesos

Mantiene muestras de CPU/RAM cada 250 ms, pero la instantánea de procesos y el
repintado periódico completo ocurren una vez por segundo. Los eventos de
entrada siguen invalidando inmediatamente la ventana. Reduce el trabajo del
propio monitor sin alterar el contador real de CPU.

## Validación

`python3 tests/kernel/test_c600_paths.py` ejecuta las funciones de producción
con ASan/UBSan: ROM con panel indirecto, referencias y punteros inválidos,
reloj efectivo, lectura/escritura USB, límites de TD, buffers con guardas,
paquetes cortos y fallos a mitad de lectura.

`tests/kernel/run_usb_perf.py` arranca una copia aislada como disco USB UHCI
con CPU Pentium III, un núcleo y 128 MiB; comprueba íntegramente una lectura de
1 MiB en dos pasadas y abre el monitor y el explorador. Guarda comando,
registro, captura y resultados. Las mediciones de QEMU no sustituyen pruebas
físicas de ATI ni predicen el tiempo de la BIOS del C600.

`python3 tools/update_hardware_image.py --usb build/bleskernos-usb-user.img`
actualiza las imágenes normal y USB mediante copias verificadas sin reemplazar
los archivos ni preferencias del usuario. Deben estar cerradas en QEMU.

## Mediciones guardadas

En `tests/kernel/evidence/c600-2026-10-10/comparison.json` se comparan la misma
imagen y carga, CPU Pentium III, un núcleo, 128 MiB y UHCI. Los tiempos de
lectura usan ticks del invitado; ambas pasadas verifican todos los bytes por
checksum. La lectura comienza 20 segundos después de iniciar el fixture para
separarla de las tareas pendientes del arranque.

| Operación | Kernel anterior | Kernel actualizado |
| --- | ---: | ---: |
| Entrada al kernel → escritorio liberado | 151.3 s | 28.72 s |
| Lectura inicial de 1 MiB | 9.05 s | 2.29 s |

La comprobación de aplicaciones del fixture inicial esperaba únicamente el
nombre `filebrowser`; el ejecutable nativo aparece como `FILE.BEX`, con su
ventana visible en la captura. Ese falso fallo se conserva en el registro de
referencia; los checksums y tiempos de transporte son válidos. El fixture
corregido acepta ambos nombres y la ejecución final pasa. La prueba de
32 MiB también pasa, incluidos abrir/cerrar contextos gráficos y abrir el
explorador, editor y monitor. No hay medición de FPS ni de modos ATI en el C600
físico: el resultado de video todavía requiere esa prueba.

Con `--check-monitor`, el runner calibra PS/2, abre Rendimiento y compara dos
capturas separadas por cuatro segundos. La ejecución final de
`monitor-verified/result.json` confirma que los gráficos se repintan. La
imagen USB y la normal tienen idénticos kernel, módulos y contenido de la
partición desde el segundo sector; el primero sólo difiere por el offset de
arranque USB. `image-verification.json` registra esa comprobación.

## Segunda ronda: protectores, latencia nativa y VRAM limitada

La puntuación de dibujo mide rasterización en RAM, mientras video mide
presentación del framebuffer; 100 en dibujo no demuestra que ATI esté acelerando
la presentación. La prueba de disco es de lectura, no de escritura. Su puntuación
ahora usa bytes/tiempo antes de redondear la velocidad a MiB/s enteros: una unidad
USB por debajo de 1 MiB/s ya no pierde toda la puntuación por truncamiento.
El monitor identifica la fila como lectura y muestra velocidades positivas
inferiores a 1 MiB/s como tales. Los errores de lectura siguen puntuando cero.

Los callbacks nativos que entraban al kernel quedaban fuera de la ruta de cesión
inmediata aplicada a workers y Linux. Yield y sleep ahora usan el cambio por
int 0x80 cuando IRQ/preempción lo permiten, marcando el contexto como seguro y
soltando/restaurando los dominios del kernel durante la espera. Se conserva la
ruta restringida para secciones no interrumpibles. No se aumenta artificialmente
la puntuación del scheduler ni del temporizador.

Los protectores Balls y Pipes amplían por vecino más cercano sin divisiones por
píxel y reutilizan filas ya ampliadas. La salida RGB sigue siendo exactamente la
misma, incluyendo pitch con padding y reducción de tamaño. En el compositor,
cada programa que invalida su contenido solicita su propio cache, también los
overlays; antes sólo se solicitaba el del propietario de la escena. Además,
el último cuadro terminado continúa visible durante un callback pendiente.
Esto evita que la animación quede estancada en el primer cache o desaparezca
cuando el render tarda más que su intervalo.

ATI reservaba un segundo framebuffer completo para presentar con BitBlt. A
1400x1050x32, dos superficies requieren 11,2 MiB; no caben en 8 MiB de VRAM.
Se agregó una ruta privada de franjas de hasta 128 filas cuando no cabe la
superficie completa. Cada franja espera que la GPU deje de leerla antes de
reutilizarla. El coordinador 2D/3D y el overlay respetan esa reserva; las APIs de
superficies completas conservan sus límites. No se leen píxeles de VRAM por CPU.
La ruta aún necesita subir los píxeles desde RAM: activar BitBlt no elimina ese
costo. La fila `GPU / HW 2D` y el informe indican capacidades dinámicas del driver,
no una medición de comandos completados por la GPU física.

Pruebas en `tests/kernel/evidence/c600-round2-2026-10-10`:

- Benchmark nativo en QEMU TCG, Pentium III, UP, 128 MiB, con un peer que despierta
  cada tick: 243 a 5318 yields/s; demora media de sleep de 29604 a 173 us,
  máxima de 29985 a 1387 us. Score scheduler 2 a 53, timer 0 a 98.
  Es una comparación emulada, no una promesa de esos valores en la Dell.
- `test_saver_scale.py`: comparación exacta de píxeles y guardas con ASan/UBSan;
  expansión 400x300 a 1400x1050, 20 cuadros, 109,91 ms antes y 39,38 ms después
  en el host sin sanitizers (2,79x). No mide el render Mesa ni la transferencia PCI.
- `test_ati_tiles.py`: VRAM simulada de 8 MiB, GPU diferida, franjas SXGA,
  reutilización segura, pitch de fuente, recorte, cursor protegido, fallo de
  envío y límite de una fila. La aceleración física ATI requiere prueba en la Dell.
- Smoke nativo de escritorio/aplicaciones con 32 MiB, VMware, KVM, SMP2 y AC97.
- `run_saver_smoke.py`: ambos protectores en copias aisladas, VMware/KVM/UP,
  128 MiB. Balls cambió 342351 píxeles entre cuadros y Pipes 133000; ambos
  volvieron al escritorio con Escape, sin excepciones de tareas ni panic.
  La primera prueba detectó que no se renovaba el cache del overlay; la versión
  final incluye esa corrección. La cobertura valida geometría coloreada y fondo
  oscuro, pues Balls lleno puede ocupar más de la mitad de la pantalla.
- USB UHCI, Pentium III emulado, UP, 128 MiB: arranque a escritorio en 30,93 s;
  lectura de 1 MiB con checksum válido en 1,46 s, repetición en cache en 0,03 s.
  El monitor siguió repintando: 1250 píxeles distintos entre muestras.

Las imágenes normal y USB se actualizan sin reinicializar la configuración del
usuario. El volumen guardado de 30% se conserva.


## Ronda 3: reloj de modo y CPU en reposo

Se encontró un error de selección de PLL: se escribía el nuevo reloj en DIV3,
pero se restauraba el selector anterior del BIOS al terminar. Si el firmware
utilizaba DIV0/1/2, el CRTC podía usar un reloj distinto al pedido, aunque las
dimensiones y el pitch fueran correctos. Ahora el modo nuevo mantiene DIV3
seleccionado; la restauración del firmware recupera el selector original.
El reloj de partida se decodifica del divisor que el BIOS tenía realmente
seleccionado, no de DIV3. Esto corrige un defecto concreto que puede afectar
la sincronización, pero no confirma todavía que resuelva la cascada del panel
físico de la Dell. Los timings siguen siendo los modos estándar existentes;
no se interpretaron registros de timings del BIOS sin una ROM real de prueba.

Las esperas 2D de FIFO, inactividad y cache tienen un límite temporal de
100 ms, además del límite de iteraciones para arranque con el reloj detenido.
Se conserva el bloqueo gráfico durante la espera: cederlo en mitad de una
secuencia de registros permitiría corromper comandos o reutilizar staging
antes de tiempo. Este cambio acota fallos y retrasos del motor; no reduce por
sí solo el costo normal de componer en RAM o subir píxeles por PCI.

`test_ati_clock_wait.py` ejecuta las funciones de producción con registros
simulados: selector inicial DIV0/1/2/3, relojes alto/bajo, preservación de bits,
fallo de commit, respuesta inmediata/demorada, timeout, wrap de ticks y reloj
detenido. Se mantienen las pruebas de pitch/estirado y staging SXGA de 8 MiB.
Referencia del selector:
[xf86-video-r128, R128RestorePLLRegisters](https://github.com/X11Libre/xf86-video-r128/blob/master/src/r128_crtc.c).

La prueba de reposo en QEMU TCG, Pentium III, UP, 128 MiB y VGA estándar dio
Vision 1% durante 10 s, 11 cuadros y un único cuadro completo. No reproduce
el 88–99% reportado en la Dell, por lo que no se declara resuelto ese síntoma.
`idle_smoke.c` verifica que Vision aparezca y quede por debajo de 20% en la
ventana de reposo. Evidencia: `tests/kernel/evidence/c600-round3-2026-10-10`.
Los scores saturados en 100 no implican tiempos constantes; video, disco y
scheduler son sensibles a redibujos, I/O y competencia entre tareas. No se
modificó la escala ni se ocultaron muestras lentas.

La segunda prueba de reposo, VMware/TCG/Pentium III/UP/128 MiB, pasó con
8 ticks de Vision en 1000 ticks (0,8%), 10 cuadros y ningún cuadro completo.
El smoke instalado de 32 MiB con VMware/KVM/SMP2/RTL8139/AC97 también pasó.
Se actualizaron y verificaron por lectura las imágenes normal y USB; su
partición coincide excepto por el campo de sectores ocultos del VBR.
`MasterVolume=30` se conserva. QEMU no emula la ATI Mobility M3; el PLL y
los límites 2D se verificaron con registros simulados, no con la GPU física.


## Ronda 4: funcionalidad basada en drivers libres

Se revisaron las implementaciones de Linux `drivers/clocksource/acpi_pm.c` y
XFree86/X11Libre `xf86-video-r128` (`r128_driver.c`, `r128_crtc.c` y
`r128_accel.c`). La integración de esta ronda es código propio para las APIs
de BlesKernOS; no incorpora archivos del kernel Linux ni enlaza el servidor X.
Referencias de implementación:

- [Linux: ACPI PM](https://github.com/torvalds/linux/blob/master/drivers/clocksource/acpi_pm.c):
  reloj fijo de 3579545 Hz, lecturas verificadas para PIIX4 y comprobación de tasa.
- [X11Libre: driver Rage128](https://github.com/X11Libre/xf86-video-r128/blob/master/src/r128_driver.c):
  offsets de tablas PLL y modos del panel M3 en el VBIOS.
- [X11Libre: CRTC Rage128](https://github.com/X11Libre/xf86-video-r128/blob/master/src/r128_crtc.c):
  formatos de píxel, ajuste horizontal para RGB565, límites VCO y precisión DDA.

### Reloj del benchmark

`kernel/arch/x86/acpi_pm.c` busca RSDP/RSDT/FADT, comprueba checksums y límites
de tamaño/rango E820 y obtiene el puerto del timer de la tabla ACPI. No se
codifica el puerto 0x808 de una Dell particular. Admite FADT clásico y GAS
extendido de System I/O; otros GAS se rechazan. Verifica tres lecturas sucesivas
con reintentos acotados y contrasta la tasa con el PIT. Contadores detenidos,
lecturas incoherentes y tasas incorrectas no se usan.

El benchmark utiliza este reloj para los tiempos, incluso al dormir o hacer
yield. Resuelve el wrap de 24 bits para intervalos largos mediante el PIT.
Sin PM válido conserva TSC/PIT y rechaza diferencias mayores al 30% para
intervalos de al menos tres ticks. La calibración de MHz sigue midiendo TSC;
no es una política de SpeedStep ni una detección completa de TSC invariante.
Los perfiles de scopes del monitor todavía usan TSC, y el porcentaje de CPU
por proceso continúa basándose en los ticks del scheduler.

### ATI: LCD, PLL y 16 bits

El PLL lee los límites de VCO de la tabla del BIOS, con fallback conservador
125–250 MHz. Descarta divisores cuyo VCO solicitado quede fuera de ese rango.
Antes sólo elegía el VCO más cercano al del arranque, pudiendo quedar por
debajo del mínimo al solicitar 121,75 MHz. Conserva la selección DIV3 de la
ronda anterior.

Se leen registros de modos de la tabla M3, con límites de ROM y validación de
reloj, geometría, sincronismos y refresco de 40–100 Hz. Sólo se aceptan cuando
la geometría de scanout coincide con la solicitada; registros expandidos hacia
una geometría distinta se rechazan. Se conservan las polaridades del catálogo.
Cuando no hay un registro válido se mantiene el modo estándar existente.
No se declara solucionada la cascada del panel hasta probarlo en la Dell.

El catálogo ATI expone 16 y 32 bits y conserva 8 bits cuando el firmware usa
modo indexado. RGB565 incluye pitch alineado, formato de CRTC/motor 2D, ajuste
de hsync y umbrales DDA escalados por reloj y bytes por píxel, recalculando la
precisión para evitar saturación. Se conserva la latencia de memoria validada
por el BIOS. La presentación convierte dos píxeles por escritura dword y hace
BitBlt final en franjas reservadas de VRAM; hay fallback CPU RGB565. Las
operaciones genéricas de píxel, relleno, lectura y BitBlt también admiten 16
bits. Las APIs públicas de superficies RGB888 y overlay mantienen sus límites.

A 1400x1050 la prueba sube 2940000 bytes en 16 bits frente a 5880000 en 32,
sin contar padding. El compositor continúa trabajando en RGB888 en RAM: no
se promete reducir a la mitad toda la memoria gráfica ni la CPU total. Para
probar esta ruta, elegir 65536 colores/16 bits en Propiedades de pantalla.
No se modificó la preferencia de color del usuario automáticamente.

Un cambio de modo fallido restaura los registros, PLL y geometría del último
modo operativo, no siempre el firmware inicial. Así no quedan las dimensiones
retenidas por el compositor en desacuerdo con el scanout tras un fallo.

### Validación

- `test_pm_clock.py`: FADT/GAS, lecturas incoherentes, wraps y rechazo de TSC
  discordante, con ASan/UBSan.
- `test_ati_lcd_depth.py`: ROM truncada/corrupta, rechazo de modos expandidos,
  conversión con guardas/alineaciones y DDA a varios relojes/profundidades.
- `test_ati_modeset.py`: catálogo 16/32 y rollback del modo anterior al fallar
  cada etapa de programación, incluyendo reinicio del motor.
- `test_ati_tiles.py`: copia diferida de GPU a 32 y 16 bits, 1400x1050, padding,
  rectángulos impares y protección del cursor en 8 MiB de VRAM.
- Se mantienen las pruebas de reloj PLL y registros de pitch/escalado.
- Smoke de arranque con 32 MiB, VMware/KVM/SMP2/AC97; benchmark Pentium III
  emulado, UP, 128 MiB. Evidencia: `c600-round4-2026-10-10`.

La aceleración ATI física y los timings reales del panel necesitan prueba en
la Dell. QEMU verifica el kernel y el reloj ACPI, pero no emula la Mobility M3.
Las imágenes normal y USB conservan configuración y volumen al 30%.
