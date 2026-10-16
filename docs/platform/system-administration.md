# Administración del sistema (API 36)

Implementación propia en `kernel/core/admin.c`, `programs/processmanager.c`,
`programs/event_viewer.c` y `system/control/`. Las definiciones, estados y eventos
se comparten mediante snapshots copiados; no se entregan punteros a las tablas
internas del administrador.

## Herramientas

Process Manager agrupa procesos e hilos, muestra CPU y memoria por proceso,
permite pedir la salida del proceso completo y cambiar su prioridad. Rendimiento
separa RAM física usada/libre/reservada, heap y frames VM. El porcentaje de un
proceso toma un núcleo como 100%. Hay una gráfica negra y verde por CPU en la
mitad superior; RAM ocupa la mitad inferior. Las cuatro secciones usan las
pestañas con relieve de Propiedades de pantalla. Un clic izquierdo abre las
acciones de la fila; abajo solo queda Finalizar en Procesos. Las listas usan
scrollbar y rueda, sin botones para pasar páginas. Los encabezados son columnas
con relieve; los gráficos y grupos tienen marcos dobles hundidos. La barra de
estado se divide en procesos/hilos, CPU y RAM, siguiendo el estilo del System
Monitor de SerenityOS.
Los hilos comparten la cifra de memoria de su proceso; no se suman para calcular
la RAM total.

Servicios muestra descripción, PID, dependencias, estado y último error. Iniciar,
detener y reiniciar son solicitudes al administrador. Vision y Wine están
protegidos frente al control manual; Vision también conserva su inicio habilitado.
PrintSpool confirma disponibilidad con su señal existente. Un servicio propio
publica `bk_service_ready(id)` desde su proceso, después de inicializar sus
trabajadores; puede reintentar mientras el administrador publica su PID.

Inicio permite crear, editar, eliminar y habilitar definiciones. Incluye nombre,
descripción, ejecutable, argumentos, fase, orden, timeout y dependencias. Las
ediciones se guardan para el siguiente arranque; el control manual de un servicio
es inmediato. Las dependencias circulares, inexistentes o de una fase posterior
se rechazan. No se elimina una definición activa ni una referenciada por otra.
Los identificadores se conservan; los huecos eliminados sin referencias se
pueden reutilizar.

El visor de eventos es una terminal de lectura que ocupa todo el cliente, sin
botones superiores ni panel separado de detalles. Los mensajes largos se
reparten en líneas y la scrollbar recorre esas líneas. Conserva búsqueda por mensaje/código,
filtros de origen, gravedad y PID, copia al portapapeles y exportación. Process
Manager abre eventos de un proceso o servicio, y Dispositivos abre eventos del
driver seleccionado. Los tiempos se muestran en segundos desde el arranque. La interfaz usa fondo
negro, texto blanco y pequeños indicadores rojos (error), amarillos (aviso) y
blancos (información). File/Edit/Help agrupa exportar, copiar, buscar y navegar;
Origen y Nivel contienen los filtros. Buscar abre un diálogo separado. El PID
filtrado se identifica en el título de la ventana. El PID de los mensajes de consola corresponde al proceso propietario del hilo; abrir
eventos de un proceso conserva ese filtro incluso cuando no hay resultados.

Los paneles nuevos Red y Aplicaciones se retiraron del Panel de control y de la
imagen. Sistema conserva Administrar, información de memoria y recuperación.
Dispositivos conserva Eventos en sus propiedades. El escritorio deja Organizar
automáticamente desactivado cuando no hay una preferencia guardada; conserva
las preferencias explícitas existentes. El visor tiene un icono en el escritorio.

## Arranque y servicios

Fases: `0` sistema, `1` escritorio disponible, `2` diferido cinco segundos después
del escritorio. Se respeta el orden dentro de las definiciones y se espera a que
las dependencias estén ejecutándose. Vision y PrintSpool mantienen su integración
con el arranque del kernel. El calentamiento opcional de Wine ahora se ejecuta
por un trabajador después de liberar el escritorio; su espera ya no retiene el
splash. Su habilitación se puede cambiar en Inicio.

Estados: detenido, iniciando, ejecutándose, deteniéndose y fallido. Un proceso
creado no confirma disponibilidad de un servicio. Las aplicaciones de inicio,
que no son servicios, se consideran lanzadas cuando se crea su proceso. El
administrador vigila todo el proceso, incluidos sus hilos, no solamente el PID
principal.

La detención es cooperativa: todos los hilos reciben una solicitud de salida.
Un hilo creado durante una solicitud hereda la salida y prioridad de su creador
al publicarse en la tabla del scheduler. El reinicio espera a que el proceso
anterior termine. Un timeout marca fallo y conserva el PID; no crea otro proceso
sobre uno que siga vivo. No se detiene una dependencia con dependientes activos.
No hay finalización forzada de procesos bloqueados en esta versión.

Prioridad baja/normal/alta modifica el quantum; no implementa prioridad estricta
ni garantías de tiempo real. Con un quantum mínimo de un tick, baja y normal
pueden coincidir. Los procesos del sistema rechazan cambios.

## Persistencia y registro

`/SYSTEM/USER/CONFIG/STARTUP.INI` usa una línea por identificador:

```
id|habilitado|fase|orden|servicio|dependencias|timeout_ms|nombre|ejecutable|argumentos|descripcion|ultimo_resultado|ultimo_error
```

Se aceptan las once columnas iniciales para compatibilidad. `dependencias` es
una máscara: el bit `id - 1` referencia ese identificador (Vision=1, PrintSpool=2,
Wine=3). Hay 16 definiciones como máximo. Campos de texto no admiten `|` ni saltos
de línea; los cambios se escriben a un temporal y se publican por rename.
Resultados y errores se guardan periódicamente y al editar la configuración.

El registro usa 128 eventos en memoria. Los productores usan un intento de lock;
un evento puede descartarse por contención para no bloquear una interrupción.
Se registran cambios del administrador y líneas de consola con prefijo entre
corchetes; no constituye un registro de auditoría exhaustivo de todas las APIs.
Un trabajador guarda snapshots acotados cada cinco segundos en
`/SYSTEM/LOGS/EVENTS.LOG`, con una generación previa `EVENTS.LOG.1`.
La exportación asíncrona crea `/SYSTEM/LOGS/EVENTS-EXPORT.TXT`; su finalización o
error se publica como otro evento. No se escribe a disco desde una interrupción.

El backend de red conserva tres perfiles, con DHCP o IP fija, máscara, gateway y DNS. Aplicar los
activa inmediatamente y guarda el perfil seleccionado y el activo. El arranque
restaura la IP fija o pide DHCP. El diagnóstico prueba resolución DNS. Un error
de persistencia después de configurar la red no revierte la configuración en
memoria; la interfaz informa que no pudo aplicar/guardar el perfil.

`/ASSOC.INI` conserva iconos y añade una tercera columna opcional:
`EXT=icono|aplicacion_abrir|aplicacion_editar`. El explorador ofrece Editar y carga
las asociaciones al abrirse de nuevo. Las rutas se validan antes de guardar.

Recuperación crea `/SYSTEM/USER/CONFIG/RECOVERY.NEXT`. El siguiente arranque borra
la marca y entra en la consola de texto antes de los drivers opcionales y Vision.
La opción prepara recuperación; no reinicia la máquina automáticamente.

## Memoria de las ventanas

Process Manager y el visor de eventos optan por `bk_gui_window_set_compact_content`:
dibujan con las primitivas GUI sobre buffers del tamaño del área cliente. Los
buffers se reutilizan y se liberan antes de crecer. Con memoria reducida se
retienen caches RGB332 y un solo slot de dibujo. Las dos aplicaciones actualizan su
geometría fuera del callback y los helpers de scrollbar no toman GUI después de dibujar;
esto evita la inversión GUI/GFX observada al abrirlo en un solo procesador.
El reaper libera también dibujos activos o en cola cuyo hilo ya murió y finaliza
las ventanas diferidas; no depende de un retorno a Ring 3 que ya no ocurrirá. Las aplicaciones históricas
que indexan píxeles directamente mantienen superficies completas.

Por debajo de 96 MiB ambas ventanas usan un tamaño inicial menor para reducir
la superficie temporal y la caché contigua; conservan las mismas funciones.
64 MiB sigue siendo un perfil con heap reducido: se verificaron las dos
herramientas de forma secuencial. Para abrir varias herramientas simultáneamente
se verificó 256 MiB. Esto no elimina el límite de memoria de otras aplicaciones.

## Verificación

Las pruebas de host ejecutan funciones de producción:

- `tests/kernel/test_admin.py`: ring acotado, snapshots, validación, ciclos,
  dependencias hacia IDs posteriores, huecos, reutilización, rollback de disco,
  resultados persistentes, disponibilidad y protección de servicios críticos.
- `tests/kernel/test_process_menu_target.py`: las acciones conservan el PID,
  servicio y pestaña originales aunque cambie la selección antes de ejecutarlas.
- `tests/kernel/test_upcall_cleanup.py`: callbacks activos o en cola al morir,
  devolución del staging y finalización única de ventanas ya retiradas.
- `tests/kernel/test_scrollbar_domains.py`: geometría privada sin bloqueo GUI,
  dibujo bajo GFX y operaciones reales de ventana todavía protegidas.
- `tests/kernel/test_process_graph_layout.py`: separación y límite de la mitad
  superior para 1 a 16 CPUs y distintos tamaños de ventana.
- `tests/kernel/test_event_filter.py`: filtro estricto de proceso, resultados
  vacíos, origen, gravedad, búsqueda y posición estable al llegar nuevos eventos.
- `tests/kernel/test_gui_content.py`: tamaño de buffers, origen, pitch, captura
  RGB332/color completo, exclusión y reutilización de slots.
- `tests/kernel/test_process_priority.py`: prioridad de todo el proceso,
  protección del sistema, quantum y herencia de prioridad/salida.

También se ejecutaron las pruebas existentes de accounting, paging, TLB, ABI
nativa y reserva del kernel. Resultados: `tests/kernel/evidence/admin-2026-10-09`.

La prueba real en QEMU usa una copia aislada del disco y un fixture Ring 3:

```
make -j4 build/programs/admin_smoke.o build/programs/ring3proxy.o
python3 tests/kernel/run_admin_smoke.py --memory 64 --cpus 1
python3 tests/kernel/run_admin_smoke.py --memory 256 --cpus 2
```

Antes, compilar kernel, stage2 y las aplicaciones modificadas. El fixture comprueba
readiness, servicio con trabajadores, reinicio con PID nuevo, detención, protección,
accounting, rechazo de un alias del kernel, perfiles de red y exportación al disco.
El runner espera el contenido dibujado, además de la creación de la ventana.
Se capturaron ambas UIs del rediseño; evidencia en
`tests/kernel/evidence/ui-redesign-2026-10-09`. La consola de recuperacion se probo en otra copia:
se activo antes de Vision y elimino la marca de arranque unico. No modifica el disco base. La imagen de uso preparada
es `build/bleskernos-admin.img`. Se puede reconstruir con
`python3 tools/build_admin_image.py`, después de compilar los componentes.
Conserva los datos y ajustes de la imagen base, agrega los accesos nuevos y
comprueba la integridad BFS4 antes de publicar la copia.

El calentamiento de Wine en la imagen existente sigue informando fallo de prime;
se registra el estado correspondiente y el escritorio permanece disponible.
Esta entrega no completa el aislamiento de todos los programas nativos: sigue
vigente la arquitectura de memoria compartida descrita en el hardening previo.

## Start y desfragmentador

Start, sus submenús y los menús del escritorio se publican junto con la caché
terminada de Vision, y se componen después de las ventanas. Un repintado de una
aplicación no puede taparlos. Abrir Start deja todas las secciones cerradas;
Este equipo se despliega únicamente al pasar por su fila o seleccionarla.

Start → Programs → Desfragmentador de disco abre `DEFRAG.BEX`. La interfaz
clásica muestra 512 cuadros, agrupando los bloques físicos de la unidad BFS4
activa. Gris indica bloques pendientes; azul, bloques leídos y verificados;
blanco, espacio libre; gris oscuro, metadatos; rojo, el bloque de un fallo real
de E/S. Cada cuadro pasa a azul cuando todos sus bloques asignados se verifican.

Analizar solo lee. Desfragmentar copia los archivos con varios extents a una
reserva contigua, compara cada bloque leído de nuevo con el original y publica
el inode antes de liberar las ubicaciones anteriores. Cada llamada mueve como
máximo 32 bloques de 4 KiB, para ceder el control entre tandas. Detener o cerrar
conserva los cambios completos y libera una copia aún no publicada. Una
escritura, renombrado, eliminación o cambio de metadatos concurrente cancela la
reubicación de ese archivo, que se registra como omitido. También se omiten
archivos cuando no existe suficiente espacio libre contiguo.

La operación conserva identificadores de inode, permisos, tamaño y fechas.
Solo el proceso que inició una operación puede avanzar o cancelarla. Las APIs
públicas `bk_device_defrag_begin/step/status/cancel` se incorporan en la versión
36; la información que recibe la aplicación no contiene punteros del kernel.
Los volúmenes FAT y CD-ROM no están habilitados para desfragmentar en esta
versión. Una unidad de solo lectura permite únicamente Analizar.

BFS4 no tiene journal transaccional. Una reserva en curso mantiene el volumen
marcado como dirty; si falla la publicación del inode, se retienen ambas copias
por precaución y se impide otra desfragmentación. Esa situación requiere revisar
la consistencia del volumen antes de continuar. Los eventos de inicio, fin,
cancelación y error se registran con origen Discos y el PID del programa.

Validación: `tests/storage/test_defrag.py` ejecuta el driver real contra un disco
de prueba, incluyendo errores de lectura, escritura, verificación y publicación,
cancelación, escrituras concurrentes, indirect extents, permisos y remontaje.
`tests/kernel/test_shell_overlays.py` comprueba composición, recorte y publicación
atómica de los rectángulos de los menús. `tests/desktop/run_defrag_smoke.py`
verifica Start sobre una ventana que sigue repintándose, Este equipo cerrado,
el lanzamiento desde Programs y una desfragmentación real en Ring 3: compara
los datos de un archivo fragmentado antes y después y verifica que queda en un
solo extent. Las pruebas siempre utilizan una copia separada de la imagen.
