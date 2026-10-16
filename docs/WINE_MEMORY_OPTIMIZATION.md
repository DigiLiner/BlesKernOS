# Optimización de memoria de Wine — trabajo en curso

No se alcanzó todavía el objetivo de WinZip 8 funcional con menos de 20 MiB
físicos incrementales. Este documento distingue cambios comprobados de trabajo
pendiente; la imagen de usuario entregada para las fases 3 y 4 conserva su kernel
anterior mientras se validan estas modificaciones en discos desechables.

## Medición

`tests/kernel/run_wine_memory.py` prepara una copia independiente del disco,
congela kernel y símbolos, instala un monitor nativo y un cliente Linux mínimo.
`wine_memory_control.py` permite capturar ventanas y provocar muestras mediante
F9 (abierto), F8 (operaciones verificadas externamente) y F10 (cerrado).
La etiqueta «functional» no certifica por sí sola operaciones ZIP.

La RAM física incremental se obtiene restando las páginas físicas ocupadas en
el heap del kernel y el asignador de frames entre la muestra basal y la muestra
con Wine abierto. El contador del heap une páginas ocupadas por varios bloques;
el de frames excluye las reservas fijas del arranque. Los contadores no suman
RSS de procesos. El RSS individual elimina alias dentro del proceso, pero una
página compartida puede aparecer en varios procesos: sus RSS no deben sumarse.
Las tablas, pilas y buffers de apoyo nuevos quedan incluidos en la diferencia
física global. El cliente de diagnóstico es el mismo en ambas muestras.

El diagnóstico también registra heap por propietario, tablas, páginas del
pager residentes/intercambiadas y reservas/compromiso de mmap y brk. Estas dos
últimas cifras describen esos subsistemas y no incluyen el ELF gestionado por
el cargador. La versión inicial del diagnóstico sumaba VMA superpuestas; el
source actual calcula la unión. Las muestras antiguas con ese contador no son
cifras válidas de reserva virtual única.

Las pruebas iniciales utilizan 1024 MiB y dos CPU con KVM. Los discos de ensayo
posteriores están en tmpfs del host para evitar demoras del disco mecánico en
flushes del journal; esto no cambia la RAM asignada al guest. Las cifras de
rendimiento no se extrapolan al disco mecánico ni a otras cantidades de RAM.

## Cambios implementados

El brk de cada proceso Linux conserva su capacidad virtual de 4 MiB, pero deja
de reservar físicamente los 4 MiB al crearlo. Las páginas se habilitan al crecer
y se asignan/ponen a cero al primer acceso. Al reducirlo se devuelven frames y
slots de swap de páginas completas. El tail parcial se limpia cuando vuelve a
crecer. Fork copia únicamente páginas materializadas o intercambiadas, no toda
la capacidad física antigua. No se aumentaron los límites del heap.

Se corrigieron comprobaciones de buffers de directorio y otras llamadas Linux:
un buffer válido que aún no está residente debe poder materializarse mediante
las funciones de copia de usuario. pread/pwrite fijan cada página durante E/S;
pread también separa correctamente COW antes de escribir. La fijación del
pager por sí sola no resuelve páginas COW gestionadas por el cache de archivos.

Los futex materializan y fijan su palabra antes de tomar el spinlock de tareas.
La comparación y el encolado siguen bajo ese spinlock; la liberación de la
fijación usa un token y una operación atómica que no puede bloquear después de
publicar el estado dormido. Se verificaron pinning concurrente y liberación en
las pruebas del pager y se repitió la suite Linux completa.

Las vistas privadas del cache de archivos pueden adoptar metadatos del pager.
Una escritura separa sólo su página de 4 KiB; las copias privadas sucias pueden
intercambiarse, conservando como fuente las páginas compartidas del inode.
Fork copia páginas privadas materializadas y conserva como referencias las
limpias. El propietario del cache sigue reteniendo sus frames completos: este
cambio no constituye todavía carga de archivos bajo demanda ni descarte de
páginas limpias. Si faltan metadatos para adoptar una vista, permanece la ruta
anterior de copia completa; no se pierde la operación por activar la mejora.

Los arrays de metadatos del pager usan preferentemente heap residente, con
propietario del kernel y etiqueta de subsistema VM. Si ese heap no puede
respaldarlos, permanece el fallback al pool de frames. Esto evita exigir un
run contiguo de frames de usuario para metadatos pequeños cuando el heap
dispone de RAM real. No amplía el heap ni modifica reservas físicas.

## Validación hasta ahora

- Pruebas del pager: PASS, incluidos rechazo de descarte de páginas fijadas,
  rollback de revocación TLB, liberación de frames/slots y recreación con ceros.
- Suite Linux i386: 30/30 PASS tras corregir la validación y pread/COW.
  Evidencia: `build/wine-memory/brk-runtime-passed/boot.log`.
- Suite Linux con futex ajustados: 30/30 PASS en
  `build/wine-memory/futex-runtime-passed/boot.log`.
- Suite Linux con copia privada por página: 30/30 PASS en
  `build/wine-memory/page-cow-runtime-passed/boot.log`. Las pruebas del pager
  verifican que escribir una página no copia su vecina, que fork preserva
  permisos y que swap devuelve los bytes de la copia privada.
- Suite Linux con metadatos del pager en heap: 30/30 PASS en
  `build/wine-memory/pager-metadata-runtime-passed/boot.log`.
- Swap real a 32 MiB, SMP2, VGA std y archivo del disco de sistema: PASS.
  10 743 salidas, 8 137 entradas, 10 130 fallos recuperados, cero errores
  y cero slots usados al terminar. Evidencia:
  `build/wine-memory/pager-metadata-swap-passed/boot.log`.
  Una prueba previa falló al reservar metadatos de fork; se conserva en
  `/dev/shm/bk-wine-page-cow-swap/32m-std-smoke-swap-file/boot.log`.
- Una ejecución intermedia falló en el caso 28 con un page fault del kernel;
  se conservó su ELF en `build/wine-memory/brk-fixed-failed-kernel.elf`.
  No se entregó ese kernel al usuario.
- El instalador original autoextraíble de WinZip falló también con el kernel
  de referencia, a 1024 MiB. Abrió WineDbg, que no consiguió adjuntarse.
  No se considera una prueba funcional ni una medición final del objetivo.
- SETUP.EXE requiere SETUP.WZ en su lista de argumentos; ejecutarlo solo
  termina sin instalar. El fixture actual incluye un BAT con esa invocación.
- La instalación directa funcionó. Se conservó
  `build/wine-memory/winzip8-installed-seed.img` y se comprobó su filesystem.
  El ejecutable extraído y el instalado son idénticos: 1 425 471 bytes,
  SHA-256 `6b5eb6b5ae70eb1cdd5eea3cee459d232488a82050ceacf1af18e91354abc3fd`.
  Desde la carpeta de extracción se completó Quick Start y se abrió la interfaz
  clásica. Desde «Program Files», una ejecución posterior falló al iniciar
  Explorer. Se está aislando esa diferencia, sin atribuirla aún a los espacios
  del nombre ni dar por cerrada la regresión.
- WinZip clásico creó `memtest.zip`, añadió README.TXT, lo extrajo en `out`
  y volvió a abrir el archivo. Después de cerrar el programa y detener el
  guest, se verificó CRC del ZIP y coincidencia exacta del miembro y del
  archivo extraído con el original de 1 891 bytes. SHA-256:
  `f52ab1c30f90912708aa34a90f5affbb6c7ba95933c879959ecc125168b5dbfc`.
  ZIP: 979 bytes. Evidencia: `build/wine-memory/winzip-source-directory/`
  y `zip-integrity.json`. Esa VM usaba brk bajo demanda y futex ajustados,
  pero todavía la copia privada antigua de vistas completas.
  Se conservó `build/wine-memory/winzip8-functional-seed.img` para repetir
  las mediciones con exactamente el mismo prefix y dataset.

## Consumo de referencia provisional

Una ejecución inicial, sin WinZip funcional, registró 14 429 páginas físicas
ocupadas antes de Wine (aproximadamente 56,36 MiB). Tras cerrar los procesos
registró 29 523 páginas (aproximadamente 115,32 MiB): alrededor de 58,96 MiB
adicionales retenidos, principalmente cache de archivos. No representa el
consumo de un WinZip abierto ni demuestra una mejora. Esa ejecución tenía una
versión inicial del cliente que no separaba correctamente los PID del argumento.

## Trabajo pendiente

Medir WinZip instalado y verificar abrir/crear/extraer ZIP con datos conocidos;
medir arranque, operación y cierre repetidos con la misma configuración.
Implementar carga por páginas de archivos/ELF: el cache aún reserva archivos
completos. La copia privada por página midió 134,34 MiB incrementales con WinZip abierto
(`page-cow-winzip`, mismo seed funcional). Abrió el ZIP y extrajo a OUT2;
el SHA-256 coincide con el original. El monitor venció antes de F8, por lo
que no existe una muestra formal de operación/cierre para esa ejecución.
Todavía no hay comparación congelada antes/después con el mismo estado del prefix.
Auditar TinyX, wineserver y servicios con datos antes de optimizarlos. Registrar
DLL, servicios, memoria compartida, swap, tiempos y regresiones de aplicaciones.

El heap identity-mapped interfiere con la base PE de 4 MiB en configuraciones
inferiores a aproximadamente 95 MiB. Reducir memoria incremental de Wine no
resuelve automáticamente esa limitación de espacio virtual del kernel. No se
promete Wine a 16–32 MiB hasta resolverla y verificarlo en el guest.

## Carga de archivos por página — validación en curso

Se agregó un respaldo de inode que no consume slots de descriptores y un
cache disperso de páginas de 4 KiB. Las vistas privadas cargan sólo páginas
usadas, comparten las limpias y copian únicamente la página escrita. Bajo
presión, las limpias pueden descartarse tras confirmar los TLB de SMP; las
privadas modificadas siguen respaldadas por swap. Las referencias sobreviven
a close, rename y unlink. La promoción a MAP_SHARED revoca primero las vistas
limpias y conserva el respaldo compartido hasta el último usuario. Se conserva
la política anterior de rechazar redimensionamientos de archivos mapeados;
no se afirma soporte nuevo de SIGBUS al truncar un mmap.

Prueba del source real: `file-pager-unit-final.log` PASS (lectura bajo demanda,
compartición, COW, fork/permisos, fallo de disco, rechazo TLB, pins, liberación,
cola parcial con ceros y promoción compartida). Guest SMP2/512 MiB:
`file-pager-runtime31-passed` **31/31 PASS**. La prueba añadida comprueba 100
inodes vivos sin crecimiento de FD, coherencia de write/rename/unlink,
liberación al último munmap y dos páginas limpias para una imagen de 9 MiB
cuyos extremos se leen; la escritura privada conserva la otra vista.

Wine está probándose con este respaldo. No hay aún muestra funcional de
WinZip con la carga de archivos bajo demanda ni consumo menor de 20 MiB.
Se corrigió posteriormente el orden VFS → GUI/Wine/Legacy en el reaper y
se evitó resolver otra vez la ruta del inode en cada page fault. Esos ajustes
y la eliminación de la copia completa del framebuffer de TinyX rootless
requieren validación adicional. La imagen principal sigue con las fases 1–4
validadas; los experimentos de Wine usan clones separados.

La prueba posterior `tinyx-context-guest-passed` pasó en 32 MiB: rootless
reserva dimensiones sin dos framebuffers completos, presenta una superficie
y rechaza un puntero inválido. Se mantiene el modo de escritorio completo.
`file-pager-swap-passed` repitió la prueba SMP2/32 MiB con 10 743 salidas,
8 137 entradas, 10 130 fallos recuperados, cero errores y slots usados=0.

`file-pager-tinyx-winzip` llegó a WinZip (primera ventana a 259 436 ms),
aceptó la evaluación, abrió el ZIP y mostró README.TXT con sus tamaños.
La muestra `open` dio **76,76171875 MiB incrementales**: 34 081 páginas
frente a 14 430 antes de Wine. Cache: 10 682 368 bytes. Esto sigue por encima
de 20 MiB; abrir el ZIP no reemplaza la validación de creación/extracción.
No es una comparación final congelada de antes/después.

La inspección de las estructuras del paginador con QMP pausado encontró
13 032 frames privados (50,90625 MiB) y 2 370 frames fuente distintos
(9,2578125 MiB), incluidos datos y código. Evidencia:
`file-pager-tinyx-winzip/pager-attribution-qmp.json`. Las DLL Unix instaladas
contienen DT_TEXTREL: por ejemplo KERNEL32.DLL.SO tiene 6 448 relocaciones
dinámicas. Wine 11.0 selecciona explícitamente -fno-PIC en Linux i386
(`configure.ac`, líneas de configuración de DLLFLAGS/LDDLLFLAGS). El enlazador
escribe esas páginas y las vuelve privadas en cada proceso.

Se preparó un build experimental con `--memory-pic` y el parche
`0008-i386-pic-memory.patch`, sin cambiar el perfil normal hasta validarlo.
El source Wine 11.0 se descargó con el SHA-256 fijado en el repositorio.
El build está aislado en `/home/lucas/.cache/bleskernos-wine-memory/pic-build`;
las librerías de enlace se prepararon con symlinks locales, sin instalar
paquetes ni cambiar las bibliotecas del host. Falta comprobar TEXTREL,
funcionalidad, memoria y compatibilidad de esta variante.

La batería `futex-mapping-runtime32-passed` aprobó **32/32** casos en SMP2
y 512 MiB. Incluye los 100 inodes, 300 ciclos, la imagen de 9 MiB,
coherencia de MAP_SHARED y futex. El caso 32 prueba dos MAP_PRIVATE sobre
un inode: despertar la segunda vista no despierta la primera, y el primer
write/COW conserva el despertar del hilo bloqueado sin FUTEX_PRIVATE_FLAG.
La identidad privada ahora es dirección virtual + dueño del address space;
sólo MAP_SHARED usa identidad física residente.

Se añadió una muestra de heap por subsistema (bytes de payload, no páginas
físicas), separada de la contabilidad global de frames únicos. Se preparó
una reconciliación explícita de foco al quitar un diálogo rootless; aún
requiere validación con WinZip. La ejecución `file-pager-tinyx-winzip` terminó
antes de completar la extracción: abrió el ZIP y el diálogo de extracción,
pero no se registra esa ejecución como prueba funcional completa.

Se implementó MADV_DONTNEED real para páginas privadas gestionadas por el
paginador (también brk), con invalidación TLB SMP antes de liberar frames
y slots. Conserva permisos y respaldo: anónimas vuelven con ceros, privadas
de archivo vuelven al inode, MAP_SHARED conserva sus datos. La prueba
`madvise-case33-passed` pasó en SMP2/512 MiB: reducción física de 16 páginas,
read-only/PROT_NONE, archivos privados, datos compartidos, agujeros y
alineación. Los hints no cargan el rango en RAM. Unit tests comprobaron
rechazo de pins/TLB, rollback y restauración del respaldo tras COW.

`baseline-functional-repeat` probó el kernel inicial congelado con el
fixture funcional: falló Explorer y no abrió WinZip. Se guarda como fallo
previo, nunca como consumo de una ejecución funcional. La siguiente
comparación usa además el primer kernel que completó operaciones ZIP,
indicando que ya contiene las correcciones iniciales de brk/futex.

Las dos repeticiones de kernels anteriores se hicieron mientras compilaba
Wine con -j2. El host tiene sólo dos CPU; el bootstrap puede agotar sus
timeouts bajo esa competencia. Se clasifican como intentos no concluyentes
para compatibilidad/rendimiento y deben repetirse en serie después del build.
No demuestran una regresión de esos kernels.

Se corrigió la clasificación de la ventana física ET_EXEC de 128–160 MiB:
su payload permanece excluido del asignador, pero pasa de «usado» a
«reservado». La cabecera del heap sigue contándose. El contador de páginas
incluye además las cabeceras de bloques libres. Esta corrección no es ahorro
de Wine: se compara el delta desde la muestra basal de cada ejecución y
se documenta la diferencia con los kernels congelados anteriores.
`reserved-accounting-unit.log` comprueba que los bytes libres no aumentan
por el cambio de clasificación y que total=usado+libre+reservado;
`reserved-heap-unit.log` conserva OOM, pins, coalescing y 1000 ciclos.

Se habilitó una marca A5 en las pilas de kernel para medir su profundidad
máxima tocada con QMP pausado; las pilas de usuario mantienen ceros.
`wine_stack_profile.py` informa capacidades por hilo/proceso y opcionalmente
la marca, sin asignaciones dentro del guest. Esa marca por sí sola no
justifica reducir límites de pila de aplicaciones o llamadas futuras.

## Checkpoint PIC y diagnóstico de pilas

El checkpoint experimental PIC conserva los 59 conjuntos de símbolos públicos
(`pic-export-audit.json`: cero símbolos eliminados). El primer clon tenía mal
ubicado el cargador interno `loader/wine`; el lanzador público es
`tools/wine/wine`, y el interno debe estar junto a `ntdll.so`. La herramienta
de staging ahora respeta esa distribución. Esa ejecución no mide WinZip.

La instrumentación inicial A5 de `mm_alloc_guarded_stack` afectaba también
buffers IPC/VFS que requieren cero; produjo un fallo de bind antes de iniciar
WinZip. Se restauró la inicialización a cero del asignador general y se limitó
A5 a `task_stack_allocate` para pilas de kernel. El kernel corregido está
congelado en `task-watermark-kernel`. Los ensayos anteriores con el kernel
`watermark-kernel` no certifican la ausencia de esta regresión.

Los perfiles QMP leen un guest detenido sin reservar memoria en él:
`wine_stack_profile.py`, `wine_dll_profile.py`, `wine_heap_profile.py`. El
perfil de DLL elimina duplicaciones físicas globales; las páginas por DLL
y PID no se suman para afirmar el objetivo. El de heap informa payload y
excluye el hueco ET_EXEC, no sustituye las estadísticas de páginas físicas.

## Limpieza de arranque normal (11 de octubre)

La marca A5 queda ahora detrás de `TASK_STACK_WATERMARK`, deshabilitada por
omisión. El lanzador normal no fuerza el monitor ni las trazas de DLL.
`WINE_STARTUP.md` documenta las optimizaciones del journal, búsquedas del
paginador y entrega de señales, junto con la validación de las imágenes.
Estas mejoras de velocidad no certifican todavía el objetivo de Wine por
debajo de 20 MiB ni el checkpoint PIC experimental, que sigue sin desplegarse.
