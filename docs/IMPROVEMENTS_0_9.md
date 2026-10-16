# BlesKernOS 0.9: mejoras estructurales

Trabajo del 11 de octubre de 2026 sobre el source local. Las cuatro fases están
implementadas y validadas dentro del alcance y los límites indicados abajo.
Vision ejecuta su motor gráfico en Ring 3 y conserva un puente para el ABI
heredado; no se afirma aislamiento total de los proxies compartidos. Las pruebas
de discos utilizaron copias temporales y QEMU `-snapshot`. La entrega de fases
3/4 conserva los datos del usuario y un respaldo completo de la imagen anterior.

## Fase 1: gestión de memoria

### Auditoría y cambios

El heap nativo es compartido, identity-mapped y contiguo. Sus límites se fijan
en boot a partir de E820 y reservas del kernel; no hay una región física libre
adicional que permita ampliarlo arbitrariamente en estas máquinas. Se conserva
este límite respaldado por RAM. El pool de frames VM se descuenta del heap:
1 MiB con 16–24 MiB de RAM, 2 MiB con 32 MiB. Páginas libres del pool no son
bloques contiguos disponibles para `kmalloc`, ni swap disponible equivale a RAM.

Cambios implementados:

- `krealloc` crece sobre bloques adyacentes, devuelve y fusiona colas al reducir,
  conserva dueño y subsistema al mover, evita overflow y mantiene el bloque
  original ante OOM. Respeta allocations fijadas por descendientes de fork.
- Lecturas nativas `fopen("rb")` usan descriptores VFS y lecturas por rango.
  Quake ya no duplica en el heap los **18.689.235 bytes** de `PAK0.PAK`.
  Cierre normal y terminación del proceso recuperan FILE y descriptor.
- En vídeo de 8 bpp las caches de pintura guardan índices de un byte por píxel;
  la superficie de trabajo conserva ARGB. Se valida la cache al cambiar modo.
- Con menos de 64 MiB, GRAPHICS.PAK permanece como tabla y archivos leídos
  por rango; el cache de iconos es acotado. El framebuffer shadow adicional
  tampoco se reserva en estos equipos. No se atribuye ahorro medido a una
  reserva que el backend ya podía omitir antes.
- Ante OOM, `kmalloc` suelta el lock del heap y recupera caches gráficos
  descartables y superficies de staging inactivas mediante trylocks; luego
  reintenta. No descarta una pintura activa ni espera otro lock bajo el heap.
- 16 MiB inicia Vision a 640×480×8; 24 MiB a 800×600×8. Las preferencias
  existentes y los modos de color soportados mantienen su funcionamiento.
- Quake usa su ventana de contenido para reducir la superficie de trabajo.
  Se corrigió `S_TouchSound` accediendo a una tabla NULL cuando no existe PCM.
- Estadísticas de heap por proceso y por subsistema: GUI, VFS, VM, procesos,
  audio, red y drivers. Los bloques todavía sin etiqueta aparecen en OTHER;
  los contadores de subsistema son payload del heap, no toda la RAM instalada.

Contabilidad: RAM instalada, heap usado/libre, mayor bloque libre, frames VM
usados/libres y reservas se mantienen separados. El total administrado incluye
metadatos del heap. Cachés reclamables siguen contando como usadas hasta su
liberación; no se suman dos veces a memoria libre. Las tablas y metadatos del
pager utilizan frames físicos de supervisor residentes.

### Evidencia

- `phase12-release-apps`: asignación de 6 MiB, integridad/realloc/liberación, 256
  aperturas/cierres gráficos Linux, cambios 8/16/24/32 bpp y tres aplicaciones
  simultáneas (File, TextEditor, Process Manager): PASS.
- Comparación con el mismo fixture RAM de 32 MiB: heap en idle pasó de
  **15.505 KiB usados / 10.990 libres** (`improvements-stream-final`) a
  **14.382 usados / 12.113 libres** (`phase12-release-apps`): 1.123 KiB menos usados.
  Es una comparación entre revisiones de esta sesión; no es un baseline limpio
  de todas las mejoras previas ni una atribución individual de cada ahorro.
- `phase12-release-16`: Vision en 16 MiB, heap idle **10.348 KiB usados / 787 libres**,
  pool VM libre 1.020 KiB con swap. No aparecen errores OOM del heap. Además
  completó la prueba paginada de 8 MiB y fork. La holgura del heap es pequeña:
  esta prueba acredita viabilidad del escritorio, no aplicaciones arbitrarias
  ni Quake con 16 MiB.
- 24 MiB: Vision y presión paginada: PASS (`phase12-release-24`).
- `phase12-release-quake`: Quake compilado, reserva nativa de **8 MiB**, demo1.dem /
  The Necropolis y progreso de frames: PASS con 32 MiB y swap habilitado.
  Heap durante Quake: **24.143 KiB usados / 2.352 libres**; swap usado **0**.
  La arena de Quake sigue residente: el éxito proviene de reducir consumo y
  corregir el audio, no de intercambiar esa arena. No se acredita una sesión
  prolongada de juego ni sonido PCM ausente en la máquina de prueba.

## Fase 2: swap y paginación real

No existía un backend de swap integrado en el source inspeccionado. Se añadió
`swap.c` y un pager por página de 4 KiB; `mm_swap_process_owners` y el cambio de
PID de tablas existentes no constituían swap.

### Respaldo y protección del disco

El backend inicial admite un disco ATA completo, escribible, de sectores de
512 bytes y base LBA cero. Solo activa discos con encabezado **BKSWAP01**,
versión 1, página 4096, geometría coherente, slots dentro del dispositivo y
CRC32 válido. Ignora particiones, otros tipos de dispositivo, imágenes de
filesystem y firmas inválidas. Nunca formatea ni escribe la firma al detectar.
Los primeros 4 KiB están reservados; un bitmap residente administra slots,
reutilización y estadísticas. El contenido de swap es temporal entre boots.

Crear una imagen dedicada (el comando rechaza sobrescribir archivos):

```sh
python3 tools/mkswap_image.py /ruta/nueva/bleskernos-swap.img --mib 32
```

Adjuntarla como otro disco IDE/ATA, separado del disco del sistema. El kernel
activa automáticamente el dispositivo validado. Las pruebas usaron IDE index 2.
La API `bk_vm_swap_enable(false)` trae las páginas a RAM antes de desactivar;
si no caben o falla una lectura, devuelve false y conserva swap activo y datos.
`bk_vm_swap_enable(true)` requiere un dispositivo previamente validado.

### Pager, sincronización y compatibilidad

- `mmap` anónimo privado usa respaldo individual, asignación física al tocar
  páginas y slots cuando se expulsa. Los PTE distinguen reservas no presentes,
  páginas residentes y páginas recuperables por el pager; el slot reside en
  metadatos del kernel, no en una dirección física falsamente presente.
- Selección inicial por recorrido circular de regiones/páginas. Solo páginas
  privadas anónimas residentes y sin pins se expulsan. No es una política LRU.
- Se retira el PTE y se exige confirmación TLB de las CPUs afectadas antes de
  escribir/reutilizar el frame. Ante una CPU demorada se restaura el PTE y se
  conserva el frame. Publicar un nuevo frame nunca lo devuelve al allocator
  por un timeout posterior. `munmap` conserva la región si falla la revocación.
- El page fault de usuario recupera páginas desde disco o demanda cero. Se
  libera temporalmente el BKL de entrada y se habilitan IRQ durante la E/S,
  evitando bloquear al dueño del mutex ATA o al IPI de otra CPU.
- Metadatos, tablas, bitmap, pilas, código y buffers necesarios para E/S
  permanecen residentes. El camino de recuperación no reserva heap. Locks:
  pager → paging (corto) / swap → backend; la asignación de tablas/IRQ usa el
  allocator físico directo, evitando el orden inverso paging → pager.
- Copias del kernel hacia/desde buffers paginados hacen prefault y pin durante
  el acceso. La E/S normal divide por páginas; pipes/sockets conservan la
  unidad de operación y atomicidad que tenía el camino existente.
- Clone privado copia los datos por página, incluyendo páginas intercambiadas;
  fija la fuente durante la copia y conserva permisos. Compartidas, mmap de
  archivo y COW existentes mantienen sus respaldos residentes y referencias.
  `vfork` utiliza el dueño real del espacio; exec cambia dueños fuera del lock
  IRQ-off del scheduler. mprotect/PROT_NONE, unmap dividido y salida están
  integrados. El último reaper devuelve frames, metadatos y slots.
- `bk_vm_statistics(pid, ...)` cuenta páginas virtuales/residentes/swapped,
  fallos recuperados, expulsiones y fallos de asignación por proceso; PID 0
  consulta totales. Slots/E/S son globales. `bk_memory_subsystems` expone heap.

El heap global nativo, imágenes, pilas, `brk`, mmap compartido/de archivo y COW
heredado siguen residentes. El pager no presenta esas reservas como expulsables.
Esta es la política inicial solicitada de priorizar páginas privadas seguras;
no convierte toda reserva contigua del kernel en memoria respaldada por disco.
Backend inicial ATA: USB/AHCI y particiones de swap no están implementados.

### Validación

La prueba de presión ejecuta un ELF Linux que escribe y verifica todos los
bytes de 8 MiB, vuelve a leer en orden inverso, hace clone privado y comprueba
isolación, E/S desde páginas intercambiadas, permisos, unmap parcial y salida
con un segundo mapping abandonado. Vision sigue activo durante la prueba.

| Prueba | Resultado |
| --- | --- |
| 32 MiB, 2 CPUs online, swap | PASS; expulsiones y page-ins reales, slots cero al terminar |
| 16 MiB, swap | PASS; 15.687 page-outs, 11.817 page-ins, 14.090 recuperaciones, cero errores/slots al salir |
| 24 MiB, swap | PASS; presión, recuperación e integridad |
| 32 MiB sin swap | OOM del proceso esperado, salida por SIGSEGV 11; Vision permanece activo y recursos recuperados |
| Swapoff con datos que no caben | Rechazo seguro, swap permanece habilitado, integridad conservada |
| Swapoff tras terminar procesos y enable | PASS |
| Fallos de disco / slot lleno / CPU sin confirmar | Rollback y conservación de frame/slot/datos: PASS en harness |
| libc Linux y Wine, 512 MiB, 2 CPUs, swap | ABI + 30/30 casos + ejecutable real Wine 11.0 `--version`: PASS |

Wine se validó con su binario real y la batería de runtime (fork/vfork/exec,
TLS, señales, futex, shared/COW, permisos, IPC y filesystem). No equivale a
probar todos los programas Windows ni una GUI completa de Wine con 32 MiB.

## Pruebas y compilación reproducibles

```sh
make -j2 build/kernel.bin build/system/services/vision.o build/stage2.bin
make -j2 build/system/control/display.o build/system/control/mouse.o build/programs/filebrowser.o build/programs/texteditor.o build/programs/processmanager.o
make -j2 -C extras/Quake-BlesKernOS-Standalone
python3 tests/kernel/test_heap_realloc.py
python3 tests/kernel/test_stdio_stream.py
python3 tests/kernel/test_shell_overlays.py
python3 tests/kernel/test_memory_accounting.py
python3 tests/kernel/test_kernel_reserve.py
python3 tests/kernel/test_paging_map.py
python3 tests/kernel/test_paging_tlb.py
python3 tests/kernel/test_swap_pager.py
python3 tests/kernel/test_native_api_abi.py
python3 tests/kernel/test_upcall_cleanup.py
python3 tests/kernel/test_vision_lifecycle.py
python3 tests/kernel/run_ram_smoke.py --ram 32 --smp 2 --swap --smoke --fixture swap --out /tmp/bk-swap
python3 tests/kernel/run_ram_smoke.py --ram 32 --smoke --fixture ram --out /tmp/bk-ram
python3 tests/kernel/run_ram_smoke.py --ram 32 --swap --smoke --fixture quake --out /tmp/bk-quake
python3 ports/wine/scripts/build-runtime.py --output build/phase12-runtime
python3 tests/kernel/run_ram_smoke.py --ram 512 --smp 2 --swap --smoke --fixture runtime --out /tmp/bk-runtime
```

El harness compila las funciones reales del heap, stdio, pager, swap y cambios
PTE; usa RAM/disco acotados e inyección de errores. QEMU prueba IRQ, ATA,
page faults reales y CPUs online. Evidencia final en `tests/kernel/evidence/`,
carpetas `phase12-release-*`; incluyendo 16/24 MiB y Quake. Se conservan boot.log, result.json y capturas. La advertencia
GNU-stack de `isr_stubs.o` y Mesa, y la función `ss_scale_rgb` sin uso en Display,
son preexistentes. Kernel: 829.420 bytes; Stage 2: 4.096 bytes. SHA256 del kernel:
`417807bf9e3fa9aa469b4623b0687849e6a0917e57b58ba2446d14bd7270f850`.
`tests/kernel/evidence/phase12-validation.json` registra resultados y checksums.

## Fase 3: BFS4

Implementada y validada sobre el driver y las herramientas reales. El diseño y
sus límites están en [BFS4_RECOVERY.md](BFS4_RECOVERY.md).

- Registro de deshacer de metadatos en los sectores reservados 3840–4081,
  con cabeceras alternas, CRC, identificación de transacción y destinos validados.
  Conserva inodos, extents, formato/versiones y archivos de imágenes existentes.
- Registro duradero antes de sobrescribir metadatos; confirmación al final.
  Montaje recupera transacciones interrumpidas antes de leer el superblock.
  Rechaza registros dañados, contenidos desconocidos y recuperación pendiente
  sobre dispositivos de solo lectura.
- Creación, escritura/extensión, truncado, borrado, rename con reemplazo,
  chmod, timestamps y sync protegidos. Desfragmentación conserva el registro
  durante la copia y cancela reservas antes de aceptar escrituras normales.
- Un fallo intenta rollback y recarga de metadatos en RAM. Si no puede
  comprobarlos, no admite mutaciones ni publica CLEAN. No libera huérfanos ni
  trunca cadenas por conjetura. Libera tablas indirectas innecesarias al truncar.
- Mutex recursivo propio de BFS4: las llamadas internas del kernel y los lectores
  no dependen de adquirir correctamente el dominio VFS para evitar carreras.
- Chequeador: CRC/identidad/tipos, referencias, padres y ciclos, nombres duplicados,
  tamaños, extents, tablas indirectas, bitmaps reservados y contadores; valida
  también montajes CLEAN. Geometría evita overflow antes de asignar buffers.
- Reparación BFS4 explícita del contador libre cuando es la única inconsistencia;
  la API de reparación ya no intenta aplicar el algoritmo FAT a BFS4.
- ATA/AHCI completan escrituras con flush; USB agrega SYNCHRONIZE CACHE(10),
  propagando errores. Editor, chequeador y extractor rechazan registros pendientes;
  `tools/bfs4.py recover` valida y recupera una imagen con QEMU detenido.

Pruebas:

- `tests/storage/test_journal.py`: PASS, 593 puntos de interrupción en diez
  operaciones, incluyendo reemplazo de destino y desfragmentación. Comprueba
  remount, metadatos consistentes y conservación byte a byte de un archivo ajeno.
- `tests/storage/test_smp.py`: PASS, ocho llamadores concurrentes, 320 ciclos
  de crear/renombrar/leer/borrar; snapshots coherentes y devolución exacta de espacio.
- Recuperación DIRTY/read-only, errores de escritura, huérfanos rechazados,
  desfragmentación y archivo swap: suites reales PASS.
- QEMU 32, 64 y 128 MiB, dos CPU y swap en el propio disco: cuatro rondas de
  doce archivos con escritura/verificación/rename/truncado/borrado, árbol eliminado
  y chequeador final sin bloques perdidos, crosslinks ni referencias inválidas.
  Evidencia: `phase3-release32`, `phase3-locked64`, `phase3-release128`.
- Una comparación inicial del espacio global y un chequeo en 64/128 MiB fallaron:
  servicios escriben archivos durante la prueba y el chequeador no estaba aislado
  de llamadas internas concurrentes. Se conservan esas evidencias negativas.
  La prueba actual separa integridad/asignaciones de su árbol de los contadores
  globales, registra el delta y pasa después del mutex propio de BFS4.

Límites: registro acotado a 120 sectores diferentes por operación; excederlo
provoca rollback/error. Protege metadatos y asignación; una escritura en los datos
existentes puede ser parcial ante fallo de E/S. La persistencia depende de que
el dispositivo respete flush. Las pruebas son cortes simulados en discos temporales,
no desconexiones físicas. Un kernel/herramienta anterior no conoce el registro:
antes de usarlo se debe recuperar y comprobar con la versión actual.

## Fase 4: Vision — implementación

El compositor, eventos, decoración, rasterización de superficies, fuentes,
gestor de ventanas, DeskManager, Deskbar y worker screensaverd se enlazan
ahora dentro de `VISION.BEX`. El servidor registra su CS en el guest:
`[VISION:ENGINE] compositor y ventanas CPL=3`. Ya no llama a una syscall que
hace el frame en CPL0. Se retiró el bucle de composición del kernel; `nm`
confirma que `gui_compositor_paint` y `gui_desktop_paint` no quedan en
`kernel.elf`. Presentación y cambios de modo permanecen en drivers.

Los mensajes de ventanas tienen tamaño fijo y llevan títulos, geometría y
valores copiados. Crear, cerrar, destruir, enfocar, cambiar tamaño/mínimos/
borde y configurar callbacks/streaming se despachan en el servidor. La
espera del cliente suelta sus dominios del kernel; salida del servidor cancela
solicitudes pendientes. Los nombres históricos de las operaciones principales
siguen resolviendo al puente compatible. Se conserva un único pool kernel de
staging para callbacks: duplicarlo en el servidor rompería la finalización del
scheduler. Los punteros heredados de ventanas siguen siendo proxies compartidos;
esto **todavía no demuestra aislamiento integral de todos los objetos GUI**.

El supervisor reinicia Vision después de su salida; el control de servicios
permite RESTART y sigue rechazando STOP del servicio crítico. Se corrigió una
carrera del monitor que podía borrar el PID del reemplazo. Los locks del frame
se abandonan al morir su dueño. Cachés privadas del servidor pertenecen a su
proceso, y las superficies GFX3D tienen registros acotados de propiedad para
liberarlas cuando termina el último hilo. Las cadenas de mount/error/idioma
que antes devolvían direcciones de BSS del kernel se exportan por copia.

Evidencia intermedia, conservada bajo `build/phase4-*`:

- `phase4-usercopy32` y `phase4-ipc32`: PASS, 32 MiB, std, dos CPU,
  tres reinicios; ventana cliente sobrevive, cambia tamaño y vuelve a pintar;
  memoria del servidor recuperada dentro de la tolerancia de 128 KiB.
- `phase4-fault32`: PASS, excepción UD2 real de Vision dentro de una
  transacción de frame, recuperación del kernel/locks/servidor y del cliente.
  Ese resultado precede las ampliaciones más recientes del ABI de mensajes.
- `phase4-vmware32`: PASS de ventanas/aplicaciones y profundidad de
  32 bpp; también precede las últimas ampliaciones del transporte.
- Se guardaron resultados negativos de callbacks tardíos y de cadenas
  supervisor devueltas a Ring 3, corregidos antes de los PASS anteriores.
- Al reducir el binario, el runner dejaba bytes viejos en la cola del último
  sector. Se corrigió el relleno y la recompilación automática de stage2;
  las pruebas afectadas se repiten en copias temporales.

Detalles de la implementación y evidencia intermedia:

- Selector de archivos y Acerca de: código de interfaz y pintura enlazado en
  Vision; trabajadores Ring 3. Se sustituyó `block_at()` por consultas por
  copia. Sus callbacks de pintura se ejecutan en el hilo compositor, y los
  cambios de estado del selector usan una transacción de dominios ordenados.
- Menús y programas de escritorio también tienen mensajes tipados. Los
  metadatos Acerca de se conservan en el proxy y sus callbacks se registran de
  nuevo al arrancar el reemplazo. Se conservan los offsets de los campos viejos.
- La cola residente de mouse conserva pulsación/liberación y posición del
  cambio; coalesce movimientos sin borrar las transiciones. Se instala el
  PS2MOUSE.DVR correspondiente. Cola acotada a 63 registros, con descarte del
  más antiguo ante saturación; el transporte conserva el estado más reciente.
- Registro kernel independiente de vida de ventanas/programas (384 entradas,
  sin seguir links del servidor). Comprueba un frame completo antes de guardar
  su orden y reconstruye las listas al morir el hilo principal. No convierte
  los proxies históricos en objetos privados ni es una prueba de aislamiento
  contra corrupción arbitraria de todos sus campos.
- `phase4-list-fault32`: PASS, 32 MiB, dos CPU, UD2 con `first_window` y
  `last_program` apuntando deliberadamente a direcciones inválidas, reinicio y
  cliente conservado; tres ciclos y heap 15.792.448 -> 15.798.492 bytes. El
  deadline de callbacks se verifica por sondeo acotado a 10 s y se registra
  latencia (se observaron 4 s con pruebas concurrentes).
- `test_vision_resources.py`, `test_mouse_queue.py`, `test_vision_ipc.py`:
  PASS de reconstrucción, clic entre cuadros, overflow y 800 solicitudes.
- `phase4-savers-dialog`: SSBALLS y SSPIPES PASS de animación y Escape. Primera
  prueba de SSPIPES demasiado temprana vio el escritorio; se conserva el
  negativo y se espera la inicialización del renderizador.
- BFS4 repitió los 593 cortes después del mutex global: PASS.
- Wine/TinyX: el diagnóstico UART identificó una recursión de #PF al
  escribir en el alias VGA 0xB8000 ocultado por las reservas DOS de Wine.
  La consola comprueba presencia/permisos antes de usar ese alias; COM1 sigue
  disponible. Las excepciones dejaron de tomar el BKL de IRQ.
- Las reservas exactas de memoria anónima admiten ahora la página compartida
  de Wine en 0x7FFE0000; el límite automático del arena no se usa para rechazar
  direcciones explícitas válidas. El límite superior sigue siendo 0x80000000
  y cada página debe tener respaldo real. La regresión verifica aislamiento
  privado al hacer fork y liberación de esa dirección.
- Se corrigieron dos deadlocks observados con Notepad real: el fallo COW
  permitía dormir con IF=0, y los sockets copiaban a páginas de usuario mientras
  conservaban el spinlock de endpoints. El handler COW habilita interrupciones
  durante la recuperación; IPC prefaulta y fija páginas antes del spinlock,
  las libera en todas las salidas y no duerme dentro de ese lock.
  `phase4-wine-cow-final/stalled.png` muestra la primera ventana real de Notepad;
  ese ensayo detectó el segundo bloqueo y **no es un PASS de Wine GUI**.
  La validación integrada más reciente se registra en el cierre de abajo.
- Se encontraron teclas de función en un `char` con signo. El campo de evento
  pasa a `uint8_t`, con el mismo tamaño/offset en GUI y SDK; pruebas de teclas
  y los binarios recompilados se repiten.

Validación adicional de la integración:

- `phase34-swap6-final`: PASS a 32 MiB/SMP2, páginas privadas, integridad completa,
  fork, mprotect, munmap y finalización. 10.750 páginas escritas, 8.162 leídas,
  10.179 fallos recuperados, cero errores de E/S y cero slots/páginas retenidos.
- Se añadió una ruta de escritura volátil para el respaldo de swap. ATA PIO
  conserva transferencia y comprobación de finalización/error de cada bloque,
  pero omite CACHE FLUSH para páginas que no deben sobrevivir al apagado.
  `block_write()` y el journal conservan su ruta durable. Otros dispositivos
  usan la ruta durable como fallback, sin cambiar el ABI de block_device_t.
  Las regresiones `test_block_writes.py` y `test_ata_swap_writes.py` pasaron
  traducción de particiones, límites, sólo lectura, errores y durabilidad.
- El broker residente conserva 32 completions de selectores: copia la ruta,
  autentica servidor y token, cancela exactamente una vez al salir Vision,
  reintenta colas llenas y libera buffers al terminar el cliente. El worker
  cierra su ventana cuando ya no existe la solicitud. La regresión del broker
  pasó; `phase4-dialog-broker-final` pasó selector, Acerca de, mouse, F11/F12
  y recuperación de los menús después del reinicio.
- Texto por píxeles, imágenes y barras de progreso se compilan con el raster
  local de Vision. Los callbacks antiguos siguen usando el puente de ABI.
- `phase34-release64`: aplicaciones/colores/SMP2 PASS; `phase34-fault128`:
  tres reinicios y corrupción deliberada de enlaces del servidor PASS.
  La primera ejecución de 32 MiB completó todas las operaciones, pero el
  runner rechazó una línea COM1 intercalada: se conservó el negativo y se
  corrigió la detección usando ejecución CPL3 más la muestra idle.
- La repetición `phase34-pending-fault32` canceló el diálogo exactamente una
  vez y recuperó pintura, pero no llegó a READY dentro del deadline de una
  de las tres vueltas bajo carga simultánea. Se conserva como fallo de
  validación; el nuevo ensayo usa un límite explícito de 60 s por arranque.

Las pruebas finales y los límites de aislamiento se registran en el cierre de
fases 3/4. Los resultados negativos intermedios se conservan para diagnóstico.

## Entrega anterior: fases 1/2 y prueba con 16 MiB

Después de las pruebas anteriores se actualizó `build/bleskernos-ata-user.img`
a pedido del usuario. Se conservó un respaldo completo
`build/bleskernos-ata-user.before-memory-swap-20261011-032523.img` y se verificó
que 5.531 inodos no modificados mantienen sus metadatos y todos sus bytes.
La imagen original tenía un bloque vacío sin referencias; se recuperó después
de validar íntegramente CRC, extents, propietarios y bitmap en una copia.
Se actualizaron boot/kernel, Vision, controles y programas correspondientes,
el header VM y este informe. No se modificaron preferencias ni archivos de Wine.

La prueba del disco instalado detectó drivers RTL8139/TLS recompilados que
necesitan `mm_set_allocation_subsystem`; se añadió su export al loader y se
recompiló el kernel. SHA256 actualizado:
`3ba2c418431268ca89442a49e4b7fd2d3a3f4f720cc7c1bb1b75976c2e5516da`.
El disco instalado pasó arranque con 16 MiB y swap identificado. Detalle de
la actualización: `build/memory-swap-image-update.json`.

Se creó `build/bleskernos-swap-32m.img`, un disco dedicado ATA de 32 MiB.
Ejecutar `./tools/run_16mb.sh` desde la raíz interna del proyecto inicia QEMU
con 16 MiB, la imagen actualizada y ese swap; conserva las modificaciones del
usuario durante el uso. El log queda en `build/qemu-16mb.log`.


## Validación final de fases 3/4

La imagen candidata final es `build/bleskernos-phase34-validated.img`. El actualizador
`tools/update_phase34_image.py` compara metadatos completos y SHA256 de cada
archivo antes/después: conserva 5.533 inodos ajenos a los diez componentes
actualizados, sin cambiar preferencias, Wine, juegos ni documentos. Escribe
stage2 y toda la reserva del kernel, incluyendo relleno cero, y comprueba BFS4.
El informe está en `build/bleskernos-phase34-validated.audit.json`.
Kernel: 800.748 bytes, SHA256
`7eb3bb656769ebf91c579239576bf38f1c9cf3b380b102e0ca73a46ec622a8c8`.

Resultados del artefacto instalado, en copias temporales:

| Configuración | Evidencia | Resultado |
|---|---|---|
| 32 MiB, std, SMP2, idle | `phase34-delivery-idle32` | PASS |
| 32 MiB, std, SMP2, apps/modos/6 MiB/GFX3D | `phase34-validated-native32` | PASS |
| 64 MiB, VMware, SMP2, apps/modos | `phase34-delivery-vmware64` | PASS |
| 128 MiB, VirtIO GPU, SMP2, apps/modos | `phase34-delivery-virtio128` | PASS |
| 16 MiB, std, SMP1, configuración mínima | `phase34-delivery-minimal16` | PASS |
| 32 MiB, SMP2, tres fallos/reinicios de Vision | `phase34-fault-serial-final` | PASS |
| 32 MiB, SMP2, swap real con DMA | `phase34-swap-low-dma-final` | PASS |
| 32 MiB, SMP2, swap desactivado/OOM esperado | `phase34-swap-off-final` | PASS |
| 512 MiB, SMP2, 30 casos Linux y Wine 11 --version | `phase34-runtime-counts` | PASS |
| 1.024 MiB, SMP2, Notepad/input/clipboard/reinicio | `phase34-wine-input-release-final` | PASS |
| 32 MiB, SMP2, Quake compilado en disco temporal | `phase34-quake-delivery` | PASS |
| Animación SSBALLS/SSPIPES y cierre con Escape | `phase34-savers-recompiled-final` | PASS |

La prueba de fallo corrompe deliberadamente los enlaces de listas de Vision y
provoca UD2 dentro de un frame. El kernel reconstruye desde su registro residente,
cancela el selector exactamente una vez, libera locks/recursos y vuelve a arrancar
Vision. El cliente mantiene ventana, redimensionamiento y pintura en tres ciclos;
heap 16.930.100 → 16.857.176 bytes. No es una prueba de resistencia a un servidor
malicioso ni a corrupción arbitraria de cada objeto compartido.

La prueba completa Linux incluye fork/COW, TLS/exec, mmap compartido y permisos,
futex, sockets/backpressure, pipes, SCM_RIGHTS, descriptores y limpieza al salir.
Un ensayo anterior (`phase34-runtime-final`) falló en la comparación de espacio
libre global de CASE25; se conserva. La repetición agrega diagnóstico sin relajar
la comprobación: 41.436 → 41.432 bloques, delta exacto de cuatro bloques, y pasa
los 30 casos. No se atribuye una causa concluyente al negativo no reproducido.

Se corrigió otro acceso a memoria de usuario dentro del spinlock de tareas:
rt_sigaction valida/fija el destino antes del lock, copia desde páginas residentes
y libera después. La entrega de señales permite recuperar páginas con IRQ
habilitadas; deshabilita IRQ antes de publicar el siguiente hilo al salir fatalmente.

El DMA ATA ya no depende de un umbral artificial de 256 MiB. Se habilita sólo
en controladores PIIX identificados, con soporte DMA y frames realmente libres;
cada canal requiere 128 KiB contiguos y conserva al menos 512 KiB para usuarios.
Si no puede reservarlos, utiliza PIO. Sólo las páginas del swap usan escritura
sin CACHE FLUSH; BFS4, journal y escrituras normales conservan flush y errores.
La prueba real a 32 MiB escribió 10.750 páginas, leyó 8.130, recuperó 10.115
faults, con cero errores de E/S y cero páginas/slots retenidos al terminar.
Sin swap, el fixture verifica OOM controlado del proceso y limpieza de recursos.

Los protectores estaban usando binarios viejos: sus reglas no dependían de
headers GUI/SDK ni incluían los .d generados. Se corrigió Makefile y se entregan
ambos recompilados. Prueba final: SSBALLS 316.739 píxeles cambiados, SSPIPES
138.951, ambos vuelven al escritorio con Escape. Los negativos anteriores de
pantalla negra/falta de animación siguen guardados.

Diez regresiones del código de producción pasaron en
`build/phase34-final-unit-checks.log`: recursos, IPC (800 solicitudes), mouse,
ciclo de vida, broker de diálogos, pager/swap, routing de bloques, ATA PIO,
ATA DMA y rt_sigaction. Verifican también errores, cancelación, permisos,
copy-on-write, particiones, durabilidad y balance de locks/pins.

### Comparación de memoria y límites

Misma configuración de 32 MiB, std, SMP2, escritorio idle; baseline es la imagen
entregada tras fases 1/2, y final es el artefacto de fases 3/4:

| Medida | Antes | Después | Diferencia |
|---|---:|---:|---:|
| RAM residente utilizada | 14.352 KiB | 14.741 KiB | +389 KiB |
| Heap utilizado | 14.348 KiB | 14.609 KiB | +261 KiB |
| Frames de usuario libres | 2.044 KiB | 1.916 KiB | −128 KiB |
| Heap libre | 12.147 KiB | 11.886 KiB | −261 KiB |

Evidencia: `phase34-baseline32-idle` y `phase34-delivery-idle32`. La separación
no reduce RAM en este punto: suma el proceso servidor, registros residentes y
128 KiB de DMA. El kernel binario baja aproximadamente 40 KiB, pero su reserva
física fija permanece igual. No se presenta eso como RAM recuperada.

En 16 MiB mínimos: heap libre 603 KiB y frames libres 892 KiB. No se aprobó la
configuración completa de VMware/SMP2/red/AC97 con 16 MiB ni Wine en 16–32 MiB.
El swap respalda páginas privadas elegibles de usuario; el heap global, pilas
nativas y reservas grandes contiguas siguen residentes. No vuelve paginable la
arena nativa de Quake. El archivo `/SYSTEM/SWAP.BKS` permanece en el propio disco,
identificado y validado; no se necesita un segundo disco.

BFS4 protege metadatos con undo journal, no garantiza atomicidad de sobrescrituras
de datos existentes. Vision mantiene proxies/callbacks de compatibilidad GUI en
el kernel; escritorio, compositor, ventanas, menús y diálogos funcionan en Ring 3,
pero falta convertir todos los objetos heredados a handles y canales privados
para aislamiento frente a corrupción arbitraria. Es trabajo recomendado para 1.0,
junto con heap nativo paginable y diagnóstico de fragmentación de Wine con poca RAM.

### Compatibilidad gráfica real de Wine y Quake

`phase34-wine-input-release-final` abre Notepad real mediante TinyX/Wine,
escribe `blesphasefour`, selecciona/copia/pega y comprueba los píxeles del
texto duplicado. Reinicia Vision mediante el control de servicios, comprueba
que el handle/owner de la ventana persiste y vuelve a pegar/escribir. PASS,
SMP2, 1.024 MiB, compositor CPL3 y pintura del cliente nativo recuperada.
Las capturas muestran el texto antes/después; F9/F8 sólo comunican al fixture
que el host ya aprobó esas comparaciones, no sustituyen la prueba visual.
El runner espera pintura estable y envía releases explícitos de teclas para
no solapar atajos mediante temporizadores de HMP. Se conservan los negativos
anteriores del comparador y las capturas que probaron pegado real.

No se aprobó una interfaz Wine con 512 MiB: un ensayo agotó una asignación
contigua de 8 MiB pese a disponer de memoria libre fragmentada. Tampoco se
aprobó interoperabilidad de clipboard nativo/X11 mediante los helpers
experimentales; no se incluyeron esos helpers en la imagen. El PASS del GUI
verifica el portapapeles de Notepad y su conservación al reiniciar Vision.

Quake se instaló únicamente en el disco temporal del fixture, a partir del
source compilado: `Quake Initialized`, arena de 8 MiB, demo y proceso activo,
PASS con 32 MiB/SMP2/8 bpp. Heap durante Quake: 24.870 KiB usados, 1.625 KiB
libres; swap activo pero uso cero. No se atribuye ese resultado al intercambio
de la arena nativa. No se añadieron juegos ni modificaron archivos ajenos en
la entrega principal.

### Archivos y áreas modificadas

- BFS4: `kernel/drivers/storage/bfs4.c`, `bfs4_journal.inc`, `bfs4_defrag.inc`,
  sus headers, reparación FAT y durabilidad USB MSC; `tools/bfs4.py`,
  `bfs4_journal.py`, `bfs4_update.py` y pruebas `tests/storage/`.
- Vision: `system/services/vision*`, motor `gui/`, UI de selector/Acerca de,
  `kernel/api/vision_ipc.c`, `vision_resources.c`, `vision_ipc.h`, puente GUI,
  exports API/loader y supervisión/cleanup de procesos.
- Raster compartido: `gui/raster.inc`, `kernel/api/gui_raster_api.inc` y
  `system/services/vision_raster_api.c`; SDK y clientes GUI recompilados.
- Entrada: mouse core, driver PS2 y USB, eventos GUI y campo de teclas SDK.
- Compatibilidad Linux/Wine: page faults/ISR, VM exacta, COW y pins,
  `kernel/linux/i386_ipc.inc`, señalización/rt_sigaction y consola VGA/UART.
- E/S de swap: block routing y ATA PIO/DMA, pager de swap, headers asociados.
- Build: Makefile, dependencias de protectores y servicio Vision.
- Entrega/validación: `tools/update_phase34_image.py`, fixtures/runners GUI,
  RAM, swap, Wine y regresiones `tests/kernel/`; este informe y
  `docs/BFS4_RECOVERY.md`. Los logs y JSON de cada resultado están en `build/`.

### Entrega instalada

Se actualizó `build/bleskernos-ata-user.img` con la imagen validada y se
verificó igualdad byte a byte por SHA256, stage2, kernel con relleno completo,
los diez payloads y el chequeador integral BFS4. Los 5.533 inodos ajenos
conservan exactamente metadatos y contenido. No se ejecutaron fixtures de
fallos ni presión de disco contra la imagen principal.

Respaldo completo: `bleskernos-ata-user.before-phase34-20261011-080848.img`.
SHA256 del disco entregado: `59f16ef76dbbb9d1b28c5c2890726200a5c0183c472b92576051813e9b32c60c`.
Informe verificable: `build/phase34-image-install.json`.

Para 16 MiB, desde la raíz interna del proyecto: `./tools/run_16mb.sh`.
Ese launcher utiliza std, un CPU y dispositivos mínimos, con swap en el mismo
disco. Para la configuración completa del comando del usuario, utilizar al
menos 32 MiB; los 16 MiB mínimos aprobados no equivalen a esa configuración.

Última comprobación después de instalar: `phase34-main-installed16`,
copia exacta del disco principal, TCG/qemu32,+rdrand/std/SMP1/16 MiB,
arranque de Vision, muestra idle y swap identificado en el propio disco: PASS.
Coincide con la configuración de `tools/run_16mb.sh`.
