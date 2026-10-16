# Arranque normal de Wine y limpieza de diagnóstico

La imagen principal es `build/bleskernos-ata-user.img`. La imagen de prueba
`build/bleskernos-wine-test.img` inicia WinZip directamente, mediante el
lanzador normal: ya no inicia el monitor de memoria ni fuerza trazas de DLL.
Las actualizaciones conservan los programas, bibliotecas y datos de cada
imagen; `tools/clean_wine_image.py` crea un backup y comprueba el sistema de
archivos antes de reemplazarla de forma atómica.

## Precarga antes del escritorio

Con más de 128 MiB de RAM física, el arranque conserva el splash `LOADING WINE` después de preparar
Vision y arrancar SMP. El kernel crea el lanzador Linux suspendido con
`--warmup`, registra su PID y sólo entonces lo hace ejecutable. Esto evita
perder una notificación rápida de READY en otra CPU.

La precarga prepara el prefijo y el tema, inicia `wineserver -p` y espera que
`wine-ready.exe` termine correctamente. Este cliente pequeño del subsistema
GUI crea y destruye una ventana oculta, un DC y un bitmap para verificar
user32 y GDI, sin iniciar una consola. El lanzador comunica ese resultado
con un ioctl que sólo acepta el PID registrado para este arranque; no se
utiliza un archivo READY persistente. Después mantiene su TinyX privado
activo para conservar las conexiones de los servicios de Wine.

El kernel publica `[WINE:BOOT] READY` antes de retirar el splash y publicar
`[BKBOOT] DESKTOP RELEASED`. Hasta entonces el compositor de Vision sigue
bloqueado por `bootsplash_active()`. Las aplicaciones configuradas para
iniciarse con el escritorio se ejecutan después de esa confirmación.

Un fallo al cargar, iniciar o preparar Wine, una salida prematura o un
timeout de 180 segundos conduce a la consola de recuperación; no publica
un escritorio como si Wine estuviera preparado. F8 conserva el acceso a
recuperación antes de iniciar servicios. La entrada de servicio Wine usa
fase BOOT, argumento `--warmup`, dependencia de Vision y estado RUNNING
sólo después de la confirmación real.

Esta precarga prepara el entorno base. Cada programa todavía carga sus
propias DLL, recursos y gráficos al abrirse. Esperar detrás del splash
traslada el coste de inicialización al arranque y mantiene procesos
residentes; no reduce por sí mismo el consumo de RAM. Las antiguas pruebas
del escritorio nativo a 16 MiB corresponden al arranque sin esta precarga.

Con 128 MiB o menos se omite la precarga, se registra `[WINE:BOOT] SKIPPED`
y se libera el escritorio normalmente. El servicio Wine queda deshabilitado.
La decisión usa la RAM física E820 (`mm_total_bytes()`), no la capacidad
del heap, la memoria libre ni el swap. El cargador también rechaza los
lanzadores habituales de Wine, Wine Tool y los ejecutables Wine/wineserver
en `/OPT/WINE/bin`, para que una aplicación configurada en autoinicio no
active Wine después de omitirlo en el boot. Las aplicaciones nativas siguen
disponibles. El límite es inclusivo: 128 MiB no inicia Wine.

Validación con KVM, CPU host, 256 MiB, SMP de dos CPU y VGA estándar:

- `wine-boot-main256`: captura del splash durante la carga, confirmación
  del estado de memoria del kernel y READY antes de retirar el splash. La
  precarga duró 26,0 s según el reloj del invitado. Alcanzó idle con 88.072
  KiB libres administrados. Se conservaron dos avisos de shootdown TLB;
  esta prueba no certifica que ese problema previo esté resuelto.
- `wine-boot-gui256`: precarga de 20,1 s y primer surface de WinZip a los
  35,5 s desde QEMU. Aceptó el diálogo inicial, abrió MEMTEST.ZIP mostrando
  README.TXT de 1.891 bytes y cerró sin reiniciar Vision. No se utilizó
  monitor de memoria, trazas de DLL ni trazas repetidas de frames. Este
  ensayo no incluye extracción ni certifica programas más pesados.
- `wine-boot-failure256`: un PE inválido en el clon provoca un fallo real
  del cliente; no publica READY ni DESKTOP RELEASED. Vision se retira antes
  de abrir la consola de recuperación, con su propietario registrado a cero.
- `test_wine_boot.py`: prueba el código real de espera con fallos de carga,
  inicio, cliente, salida prematura, timeout, carrera entre salida y READY,
  registro antes de ejecución, wrap del reloj y frecuencia cero.

Los resultados están en `build/wine-memory/wine-boot-*`. El observador del
arranque usa QEMU `-snapshot`, lecturas QMP del estado del kernel y capturas;
no escribe en el disco suministrado. Sus pausas afectan el tiempo del host.
Los tiempos anteriores de 1024 MiB corresponden a otras configuraciones y
no son una comparación directa de rendimiento con esta precarga.

La política de RAM se comprobó en `wine-ram-policy32` y
`wine-ram-policy128`: escritorio operativo, ausencia de LOADING WINE/READY,
PID de precarga y resultado de Wine en cero, y rechazo del autoinicio de
WinZip antes de cargar su lanzador Linux. `wine-ram-policy256` conserva la
precarga real, READY antes del escritorio y TinyX/Wine residentes.
`test_wine_boot.py` comprueba el código real de política y del bloqueo de
lanzadores justo por debajo, exactamente y por encima de 128 MiB, además
de 16, 129 y 256 MiB; también acepta aplicaciones nativas y rutas con otras
mayúsculas. Esta regla no implica que todas las aplicaciones Wine funcionen
con cualquier cantidad de RAM superior al umbral.

## Cambios

### Configuración completa VMware, RTL8139, AC97 y CD

La corrección posterior al log del usuario se prueba con KVM, CPU host,
256 MiB, dos CPU, VGA VMware, red RTL8139, AC97, periféricos por defecto y
`media/installers/Quake2.iso` en IDE. Los observadores capturan la salida
serie en un archivo; el frontend se elige con `--display gtk` o `none`.

- El scheduler SMP ya no publica `running_cpu=-1` para hilos dormidos o
  terminados antes de salir de su pila. Los mantiene en HANDOFF, igual que
  los hilos ejecutables, y publica la retirada bajo el lock de tareas en la
  siguiente entrada del CPU. Esto impide que otro CPU despierte un futex o
  libere la pila mientras el syscall anterior todavía restaura registros.
  `test_smp_sleep_handoff.py` ejecuta el código real de publicación y despertar
  y comprueba también la carrera de salida de un hilo pendiente.
- Los lanzamientos nativos síncronos usan los dominios TASK/VFS/GUI; los
  lanzamientos asíncronos y el callback del driver de Vision utilizan su
  sincronización interna. El reaper incluye GFX antes de WINE/LEGACY.
  Se corrigen las inversiones observadas al abrir aplicaciones y retirar
  superficies, manteniendo los avisos para otras entradas sin clasificar.
- Las páginas DOS privadas pueden empezar en 0x1000. La página cero sigue
  rechazada antes de modificar mapeos; los rangos vivos del kernel y MMIO
  siguen protegidos. Wine reservaba 0x1000..0x10000, pero el reemplazo final
  0x1000..0x110000 fallaba por el antiguo mínimo de 64 KiB después de retirar
  el mapa existente. Esto causaba el fallo de escritura en 0xf0400 del
  instalador Win16 de Quake II.
- `mprotect` de reservas anónimas ahora crea regiones del paginador y asigna
  RAM al tocar páginas. Antes materializaba toda la reserva contiguamente,
  y fork duplicaba ese respaldo incluso para grandes regiones sin utilizar.
  El fork anónimo sigue copiando páginas privadas presentes; no se afirma
  que todas las páginas anónimas dispongan de COW compartido.
- Los mapas privados de archivo pueden reservar también el extremo de BSS
  posterior al EOF. El cargador ELF lo reemplaza con memoria anónima sin
  necesitar una gran reserva física contigua. El último bloque parcial se
  rellena con cero; acceder a páginas completas fuera del archivo entrega
  SIGBUS/BUS_ADRERR. PROT_NONE conserva SIGSEGV.
- Un diagnóstico de retorno de futex registra únicamente resultados
  inesperados de esperas que realmente se registraron. No imprime cada
  syscall ni los retornos normales. Se retiraron las trazas DOS usadas para
  localizar el fallo.

`user-hardware-smp-main/functional.json` comprueba tres tandas de ocho hilos
(432.000 incrementos protegidos), una reserva virtual de 128 MiB con fork
aislado, protección de página cero/kernel vivo, EOF/permisos/BSS y el
instalador del CD. El instalador mostró bienvenida y licencia, se canceló
y se cerró Ejecutar sin reiniciar Vision. No se instaló ni se ejecutó el
juego; el primer surface no mide el tiempo de instalación. No se observaron
abortos de futex, Bad file descriptor, fallos de fork ni inversiones de locks
en ese ensayo. La instrumentación QMP conservó un aviso de shootdown TLB;
ese problema previo no se presenta como resuelto.

La prueba inicial `user-hardware-sync` verificó manualmente las mismas
páginas DOS y mostró los diálogos, pero su observador buscaba el título
`setup` en lugar del nombre real `_ins576._mp` y agotó su timeout. No es un
resultado automático PASS. La repetición final usa el título correcto.

Las pruebas se hacen en clones; la publicación preserva programas y datos
de las imágenes originales, mantiene la regla de no iniciar Wine con
128 MiB o menos y crea backups antes del reemplazo.

- Wine usa `WINEDEBUG=-all` normalmente. La salida de inicialización de TinyX
  se silencia en ese modo. El lanzador conserva la detección de fallos.
- Vision deja de imprimir una línea por cada frame lento; mantiene los
  contadores de rendimiento. Se puede compilar con `VISION_FRAME_TRACE`.
- El prefijo desactiva la exportación Linux XDG de asociaciones MIME que
  Vision no utiliza. Conserva las asociaciones Windows y sus claves.
- El paginador guarda búsquedas recientes y los intervalos sin mapeos. Los
  buffers del servidor gráfico y las pilas ELF ya no recorren la lista global
  por cada página. Las modificaciones de mapeos invalidan la caché bajo su
  mutex; los rangos que cruzan límites mantienen la comprobación completa.
- El journal reúne hasta cuatro registros contiguos por escritura durable.
  Publica su cabecera después de escribir los datos anteriores. Conserva las
  barreras, el formato de disco y la recuperación; utiliza 4 KiB residentes.
- La marca A5 de las pilas del kernel es opcional (`TASK_STACK_WATERMARK`).
  Las compilaciones normales mantienen la inicialización a cero.
- La preparación de señales Linux permite interrupciones mientras copia o
  materializa el frame de usuario. El retorno final y la selección de otra
  tarea siguen con interrupciones deshabilitadas. Antes, una espera por el
  mutex del paginador desde ese retorno podía bloquear Wine en SMP.

Para diagnóstico explícito, desde un entorno Linux del sistema:

```sh
WINEDEBUG=err+all /WINEGATE/bin/wine-launch --diagnostics Z:\\ruta\\programa.exe
```

No hace falta habilitar diagnóstico para ejecutar aplicaciones normalmente.
No se desplegaron los módulos PIC experimentales en estas imágenes.

## Evidencia

Las mediciones usan KVM, CPU host, VGA estándar, 1024 MiB, dos CPU y clones
separados del mismo prefijo. Miden el primer surface visible de WinZip desde
el arranque de QEMU, con observación QMP cada cinco segundos. No representan
el tiempo de todas las operaciones posteriores. Las comparaciones son
orientativas: hay variación de planificación y de caché del host.

| Compilación | Primer surface | Flush del disco |
|---|---:|---:|
| Lanzador limpio, kernel inicial (`quiet-winzip-final`) | 175,5 s | 55.570 |
| Journal agrupado y caché positiva (`quiet-fast-winzip`) | 151,1 s | 17.491 |
| Caché de intervalos ausentes (`quiet-gap-winzip`) | 70,6 s | 6.998 |
| Imagen entregada (`quiet-delivery-winzip`) | 60,5 s | 7.511 |

Resultados y capturas: `build/wine-memory/<nombre>/result.json`.
La repetición `quiet-gap-confirm` abrió el surface en 88,6 s, pero sufrió
una espera por el mutex en la entrega de señales. No certifica funcionalidad
SMP; motivó la corrección de señales incluida en la compilación final.

La prueba del journal incluye 184 interrupciones por solicitud y 822 por
sector, además de 320 operaciones concurrentes. Las pruebas del paginador
comprueban presión, integridad, permisos, COW, mapeos privados/compartidos,
invalidación de caché y liberación. El test de señales comprueba la posibilidad
de bloquear al construir el frame y el retorno atómico al scheduler.

El núcleo final también pasa los 33 casos Linux con dos CPU
(`quiet-final-runtime-evidence`), las aplicaciones nativas y profundidades
8/16/24/32 bpp con 32 MiB (`quiet-final-native32-evidence`), y swap real con
32 MiB y dos CPU (`quiet-final-swap32-evidence`): 10.743 expulsiones,
8.137 lecturas, 10.130 fallos recuperados, cero errores y cero slots retenidos
al terminar. El escritorio mínimo de 16 MiB arrancó con una CPU, VGA estándar,
`-nodefaults` y swap en archivo (`quiet-final-native16-evidence`). Esto último
no certifica Wine con 16 MiB ni la configuración completa con audio/red/SVGA.

También se corrigió la traducción del argumento de escritorio de los imports
históricos `gui_desktop_*` en el puente ELF. La vista de Ring 3 debe resolverse
al escritorio real antes de retirar o reordenar ventanas. Un cierre desde
“Ejecutar” retiraba la ventana de la vista vacía y después liberaba un objeto
que Vision aún tenía enlazado; su comprobación detectaba el error y reiniciaba
el servidor. `quiet-runbox-regression/close-result.json` comprueba el cierre
con dos CPU sin reinicio, conservando Vision PID 5. El test del puente mantiene
argumentos originales, escritorios de preview, dominios, errno y ABI de retorno.
La compilación que incluye esta corrección volvió a pasar los 33 casos Linux
(`quiet-delivery-runtime-evidence`) y las pruebas nativas de 32 MiB
(`quiet-delivery-native32-evidence`).

La prueba `quiet-final-winzip/functional.json` abrió MEMTEST.ZIP y extrajo
README.TXT a `Z:\SYSTEM\TEST\WZ8\OUTFAST`: 1.891 bytes idénticos al miembro
del ZIP, SHA-256 `f52ab1c30f90912708aa34a90f5affbb6c7ba95933c879959ecc125168b5dbfc`.
Ese ensayo detectó después el fallo nativo de cierre que se corrigió en el
puente; no se presenta como una prueba sin reinicios.

La imagen entregada se comprobó contra el kernel congelado
`quiet-delivery-kernel` y el lanzador normal: `quiet-delivery-images.json`
registra backups y SHA-256. El arranque final conservó un aviso aislado de
shootdown TLB; los errores reales no se ocultan. El inicio en frío de Wine
sigue siendo aproximadamente un minuto, y no se certifica todavía carga
instantánea ni el objetivo de memoria inferior a 20 MiB.

Para repetir la configuración de WinZip validada desde la raíz del repositorio:

```sh
qemu-system-i386 -accel kvm -cpu host -smp 2 -m 1024 \
  -drive file=build/bleskernos-wine-test.img,format=raw,if=ide \
  -vga std -display gtk -nodefaults \
  -serial file:build/wine.log -no-reboot -no-shutdown
```

La prueba de entrega `quiet-delivery-winzip/functional.json` también abrió
MEMTEST.ZIP, extrajo README.TXT a OUTFAST con el mismo hash y cerró WinZip,
con dos CPU y el lanzador normal. Vision conservó PID 5 y no reinició; el
archivo fue comparado directamente con el miembro del ZIP después de parar
el clon. Las capturas y el resultado del arranque de 60,5 segundos corresponden
al mismo kernel instalado en las dos imágenes entregadas.

El arranque mínimo de la imagen principal instalada se repitió con 16 MiB,
una CPU, VGA estándar y TCG, usando `-snapshot` para conservar el disco
original: `quiet-delivery-native16-installed/result.json`. El escritorio
arrancó y alcanzó la muestra idle con 1.667 KiB libres. No se inició Wine en
ese ensayo.
