# Wine 11 en la imagen normal

El build ATA `user` y `developer` incluye Wine 11.0 i386, TinyX/KDrive y las
bibliotecas i386 del paquete `ports/wine/package/wine-11.0-i386/rootfs`.
`make all` genera también USB y VMware a partir de esa imagen ATA. La imagen
FAT32 ahora reserva 768 MiB. No necesita el árbol de compilación de TinyX
en `/tmp` para generarse: `XFBDEV` validado está en el paquete.

El explorador usa `Associations.INI`: `EXE` y `BAT` llaman a `WINE.BEX`.

El contenido visible de la unidad `C:` se guarda directamente en
`/SYSTEM/WINE`. El `WINEPREFIX`, registro y runtime Unix siguen bajo
`/WINEGATE`/`/OPT/WINE`; `dosdevices/c:` y `$WINEPREFIX/drive_c` apuntan a
`/SYSTEM/WINE`. De esta forma los programas instalados por Wine son archivos
BFS normales y Archivos ofrece un acceso rapido «Programas de Windows».
«Ejecutar» usa la misma ruta. `WINE.BEX` inicia el ELF i386
`/WINEGATE/bin/wine-launch.ELF` con la ruta como argumento. Cada lanzamiento
crea un TinyX privado en un display libre. El launcher registra cada ventana
X11 principal en `/dev/blesgfx`, que crea una ventana Vision con su tamaño,
título y marco nativo.
La imagen incluye un prefijo `/WINEGATE/prefix` ya inicializado y validado en
KVM. En el primer lanzamiento, `WINE.BEX` crea los enlaces FAT `C:` y `Z:` que
el empaquetador de imágenes no puede copiar. El prefijo persiste en el disco.
Si se borra manualmente, el lanzador intenta reconstruirlo con `wineboot --init`,
lo que puede tardar varios minutos. El lanzador escribe `disable` en
`.update-timestamp` para evitar que Wine repita la actualización del prefijo
por las fechas de FAT.

Panel de control → Wine abre directamente `winecfg`, con sus ajustes reales y
la pestaña «About» de Wine 11. Los `.bat` usan Wine `cmd /c`.
El marco, movimiento, minimizar y cerrar son de BlesKernOS. El prefijo de
fábrica ya contiene la paleta clásica de BlesKernOS para controles Win32
estándar. Los programas que dibujan sus propios controles conservan su diseño.

`make all` completó las imágenes ATA, USB y VMware. La imagen ATA nueva llegó
a `VISION ONLINE` en KVM con dos CPU, AHCI y 1536 MiB. En una imagen KVM con
prefijo Wine ya inicializado, el ejecutor común abrió un `.exe` y un `.bat` a
través de `WINE.BEX`; ambos acabaron con código 0. La prueba de `.bat` imprimió
`BLES_WINE_BAT_OK`. Los resultados están en
[`evidence/wine-associations-result.json`](evidence/wine-associations-result.json).

Notepad se mostró en una ventana Vision de su propio tamaño, con título y
botón en Deskbar; la importación del tema clásico devolvió 0. La captura está
en [`evidence/wine-rootless-notepad-native.png`](evidence/wine-rootless-notepad-native.png).
Una copia limpia de la imagen final abrió Notepad en su primer arranque, con
tema clásico y el borde inferior por encima de Deskbar; la captura está en
[`evidence/wine-preseed-notepad-native.png`](evidence/wine-preseed-notepad-native.png).
El resultado de la prueba está en
[`evidence/wine-preseed-native-result.json`](evidence/wine-preseed-native-result.json).
Con el nuevo modo rootless, el escritorio queda libre mientras carga Wine;
[`evidence/wine-control-loading.png`](evidence/wine-control-loading.png)
muestra la espera y [`evidence/wine-control-about.png`](evidence/wine-control-about.png)
la pestaña «About» de `winecfg` ya abierta.
La prueba en frío del Panel está resumida en
[`evidence/wine-control-rootless-result.json`](evidence/wine-control-rootless-result.json).
El prefijo empaquetado procede de la validación anterior; su procedencia y SHA256 están
en [`../wine/package/wine-11.0-i386/prefix-template.json`](../wine/package/wine-11.0-i386/prefix-template.json).

## Integración de ventanas Wine/TinyX

El bridge conserva un framebuffer X11 de 760×536 por instancia y presenta el
rectángulo de cada ventana X11 en su propia ventana Vision. La entrada se
traduce de coordenadas Vision a las del framebuffer X. Una ventana mayor que
760×536 se recorta al área disponible de TinyX.
La posición inicial se limita al área libre sobre Deskbar; el ancho y alto
solicitados por cada aplicación se conservan.

Para reconstruir el launcher usado por la imagen normal:

```sh
bash ports/tinyx/scripts/build-desktop-launcher.sh
```

## Rootless v2: una ventana Vision por top-level X11

`wine-launch` usa un TinyX sin ventana contenedora en Vision. El root X sigue
existiendo como framebuffer de compatibilidad de 760x536, pero el launcher
enumera sus hijos `InputOutput` mapeados. Cada top-level no
`override_redirect` se registra con el kernel mediante
`BLES_GFX_SURFACE_UPSERT`, usando el XID como `surface_id`.

El bridge crea una ventana Vision independiente para cada superficie y, en
cada `BLES_GFX_PRESENT`, copia solamente el rectángulo correspondiente del
framebuffer X11 al contenido de esa ventana. Durante la carga no aparece
ninguna ventana TinyX. Por eso un proceso que
cree dos ventanas (o un EXE que lance otro EXE dentro de la misma instancia)
puede producir dos botones/ventanas reales del escritorio sin levantar otro
servidor X.

La entrada hace el camino inverso: las coordenadas locales de cada ventana
Vision se traducen a coordenadas del root X. Close, resize y focus se envían a
`wine-launch` mediante `BLES_GFX_SURFACE_EVENT`; el launcher traduce close a
`WM_DELETE_WINDOW`, resize a `XResizeWindow` y focus a `XSetInputFocus`.
Los diálogos `WM_TRANSIENT_FOR` se posicionan relativos a su padre, bloquean
su ventana padre mientras están activos y usan el marco nativo de Vision.

### Apariencia BlesKernOS

El frame externo (título, borde, mover, minimizar, cerrar, foco, resize y
entrada en Deskbar) es de Vision. En las capturas de Wine actuales las
ventanas X11 de Notepad/diálogos empiezan directamente en el área cliente, por
lo que no se duplica una segunda barra de título Win32.
`XFixesHideCursor` oculta el cursor que TinyX dibujaría en el framebuffer;
el puntero visible es sólo el de BlesKernOS.

Como capa adicional, el prefijo de fábrica tiene una paleta clásica basada en
los colores CDE de BlesKernOS en `HKCU\\Control Panel\\Colors` y el tema visual
desactivado mediante `ThemeManager/ThemeActive=0`. El lanzador aplica esos
valores a prefijos creados de nuevo. Esto afecta a
controles Win32 estándar que respetan los colores del sistema; una aplicación
que dibuja su propia interfaz seguirá conservando su diseño.

Limitación conocida: ventanas X11 `override_redirect` (menús popup/tooltips)
no reciben una ventana Vision propia en esta versión. Se siguen viendo si
quedan dentro del rectángulo de su top-level; un popup que sobresalga fuera de
ese rectángulo puede quedar recortado. Es el siguiente caso a convertir en una
superficie overlay nativa.


## Photoshop 4: caída del servidor al avanzar en el instalador

El registro del 8 de octubre de 2026 muestra SIGSEGV de XFBDEV en
`0x080ee873`, dentro de `DoGetImage.constprop.0`, al ejecutar un `push` antes
de enviar la respuesta X11. La reserva local anterior usa `alloca` para hasta
64 KiB (`IMAGE_BUFSIZE`); XFBDEV dispone de una pila de usuario de 64 KiB, que
también contiene los marcos de llamadas. El fallo del servidor rompe las
conexiones de todos los procesos Wine de ese lanzamiento.

`DoGetImage` ahora reserva su búfer temporal en heap y lo libera al terminar.
Conserva la transferencia por bloques y devuelve `BadAlloc` si no hay memoria;
el caso de imagen vacía reserva un byte para no depender de `malloc(0)`.
La variante que devuelve una imagen completa conserva su propiedad de memoria.

`python3 ports/tinyx/tests/test_getimage_stack.py` ejecuta el manejador real con
pila protegida de 32 KiB: imagen 800×600, fila de 80 KiB, XYPixmap, retorno de
imagen completa, error de asignación y liberaciones equilibradas. La variante
anterior con `alloca` reproduce SIGSEGV en esa misma prueba; la corregida pasa.
Esto valida el mecanismo del fallo, no la instalación completa de Photoshop.
`make user-update` instala también el XFBDEV del paquete sin reconstruir el
prefijo ni borrar programas instalados.

## Photoshop 4: transferencia desde los volúmenes del CD

El siguiente registro ya no muestra la caída de XFBDEV; InstallShield informa
error de transferencia -2 al llegar a `bbrd11.bmp`. El CD contiene los archivos
`DATA.1` a `DATA.22` distribuidos entre directorios `DISK1` a `DISK22`.
El mensaje no identifica por sí solo la operación que falla.

El lanzador ahora asigna `/CDROM` a una unidad Windows (D: si está libre) y
convierte la ruta del ejecutable a esa unidad. Reutiliza asignaciones existentes
y conserva las unidades del usuario. Esto permite resolver rutas relativas a la
raíz del CD sin usar Z:, cuya raíz es la del sistema completo.

Las consultas de espacio del CD ya no devuelven la geometría del disco del
sistema. `statfs64` y `fstatfs64` informan ISO9660, bloques de 2048 bytes, sólo
lectura y espacio libre cero. El kernel registra búsquedas fallidas de archivos
`DATA.n`/`DATA.Z` con `[LINUX:INSTALL-VOLUME]`, limitadas a 32 rutas distintas
por arranque, para identificar el volumen solicitado si el error continúa.

`python3 tests/desktop/test_optical_installers.py` prueba las funciones C reales
con backends simulados: geometría y ABI, unidades ocupadas, reutilización de
asignaciones, ausencia de CD y conversión de rutas. La instalación completa y
el cambio de volumen de InstallShield todavía requieren confirmación en QEMU.

## Rendimiento de instalación desde CD

ISO9660 mantenía `g_iso_bulk_reads` desactivado desde el montaje. Cada sector
necesitaba un comando ATAPI independiente y las lecturas pequeñas y búsquedas
de directorio repetían el acceso físico, incluso dentro de un mismo sector.
Ahora se permiten solicitudes de hasta 32 sectores (64 KiB), con recuperación
al modo de un sector si la unidad rechaza la lectura agrupada.

Una caché compartida de 512 sectores, aproximadamente 1 MiB, conserva datos y
directorios con reemplazo por antigüedad dentro de grupos de cuatro entradas.
No retiene descriptores VFS ni mantiene el spinlock durante operaciones de disco.
El montaje invalida sus entradas; una generación evita publicar lecturas viejas
después de esa invalidación. Si no hay memoria, las lecturas directas continúan.
El aviso AC97 de DMA directo aparece una vez por inicialización del dispositivo.
`make user-update` instala también ISO9660.DVR y AC97.DVR junto con el kernel.

`python3 tests/desktop/test_iso_read_cache.py` compila el lector C real con
AddressSanitizer y UndefinedBehaviorSanitizer y un dispositivo simulado que
cuenta operaciones: 64 KiB requieren un comando agrupado, 128 lecturas pequeñas
de un sector requieren un comando, y 100 búsquedas repetidas de un directorio
requieren un comando. También comprueba bytes, límites de archivo, rechazos de
lectura agrupada, errores transitorios y permanentes, reemplazo, invalidación y
falta de memoria. Estos resultados miden operaciones de almacenamiento; no
representan una medición del tiempo total de Office o Photoshop en QEMU.

## Detección intermitente del CD al arrancar

Las esperas PACKET comprobaban ERR/DF antes de que BSY se despejara y tenían
límites por cantidad de sondeos más cortos que el resto del transporte ATA.
Ahora sólo interpretan errores con BSY desactivado, tienen un plazo de cinco
segundos con límite de sondeos para arranque sin temporizador, y ceden CPU
cuando el scheduler y las interrupciones lo permiten. IDENTIFY, IDENTIFY PACKET
y PACKET respetan también la pausa de selección/comando antes de leer estado.

READ CAPACITY recupera fallos con REQUEST SENSE y hasta tres intentos. Esto
consume UNIT ATTENTION después del reset o cambio de medio. Una bandeja vacía
(NOT READY / MEDIUM NOT PRESENT) termina inmediatamente esa consulta. Las
respuestas incompletas o geometrías inválidas no se publican como capacidad.
ISO9660 publica el estado montado sólo después de comprobar medio, descriptor
y raíz; un intento fallido permite volver a montar posteriormente.

Si el montaje inicial falla con `cd0` detectado, la tarea `cdrom-mount` hace
hasta 20 reintentos separados por medio segundo, bajo el dominio VFS, y termina
al montar. No ejecuta sondeos desde callbacks gráficos ni crea esa tarea cuando
no hay unidad detectada. El plazo de cada operación de transporte sigue siendo
independiente del intervalo entre reintentos.

`python3 tests/desktop/test_atapi_ready.py` prueba las funciones C de espera,
recuperación de capacidad y tarea de montaje con estados simulados: BSY con
errores residuales, respuesta demorada, timeout, UNIT ATTENTION, medio no listo,
bandeja vacía, geometría inválida y reintentos limitados. La prueba de caché ISO
comprueba además que un montaje fallido no se publica y que un intento posterior
puede recuperarlo. Falta confirmar la frecuencia de detección en arranques
reales repetidos de QEMU.

## Photoshop 4: bucle al confirmar los datos de registro

La reproducción en QEMU/KVM con una copia del disco mostró solicitudes Wine
`set_cursor` alternando `SET_CURSOR_NOCLIP` (0x10) y
`SET_CURSOR_CLIP | SET_CURSOR_FSCLIP` (0x28), intercaladas con `get_message`.
El backend X11 devolvía éxito y llamaba a `NtUserClipCursor(NULL)` cuando TinyX
no ofrecía XInput2. Al recibir esa notificación, win32u intentaba confinar de
nuevo el cursor a la ventana de pantalla completa. Esa realimentación ocupaba
CPU y dejaba un rectángulo negro después de confirmar el registro.

`ports/wine/patches/0005-winex11-no-xinput2-clip-loop.patch` devuelve fallo sin
generar otra notificación. No altera la ruta con XInput2 disponible. El
confinamiento físico del cursor sigue sin estar disponible en esta ruta de
TinyX; no se anuncia como una nueva capacidad. El paquete incluye el backend
`winex11.so` recompilado, y el actualizador lo instala en la imagen existente.

`python3 ports/wine/tests/test_cursor_clip_loop.py /ruta/a/wine-source` compila
la ruta real anterior a las llamadas XI2 y simula el reintento de pantalla
completa: la versión corregida termina en un intento y la versión original
reconstruida falla la prueba por realimentación.

También se corrigió el sondeo de canales ATA con maestro ausente y CD esclavo:
el estado 0xff del dispositivo seleccionado no descarta todo el canal. La
prueba `test_atapi_ready.py` comprueba que se sondean ambas posiciones y se
encuentra el esclavo. Esto se reprodujo durante los arranques de la prueba.

Validación en QEMU/KVM, 2 CPU y 1536 MiB, disco AHCI y media/installers/PH4.iso: la misma
confirmación que antes dejaba el rectángulo negro ahora abre la selección
Typical/Compact/Custom y directorio de destino. Esta comprobación valida el
bloqueo de registro; no certifica una instalación completa de Photoshop.

## Quake II: rechazo del idioma al pulsar Install

El aviso de idioma del CD no era una caída del proceso. La edición probada
rechaza inglés estadounidense/canadiense. Se comprobó con un ejecutable Win32
que Wine informa LCID 0x0409 y que `GETINFO.DLL!IsEnglishUS()` devuelve 1.
La misma instalación abrió la bienvenida al usar LC_ALL=en_GB.UTF-8, sin
cambiar la versión de Windows ni los archivos del juego.

El launcher usa en-US explícito por defecto: sin archivo de locales de glibc,
Wine puede usar LC_ALL como fallback cuando libc permanece en C. Para este
instalador selecciona en-GB únicamente si SETUP.INS coincide en tamaño y
huella FNV-1a con la edición comprobada (67519 bytes, 0x5295e9dd). Se detectan
SETUP.EXE en la raíz del CD y en INSTALL. Otros instaladores y ediciones no
reciben ese ajuste. El entorno se hereda por los subprocesos del instalador;
no se cambia el idioma del escritorio ni se escribe el registro del usuario.

`python3 ports/wine/tests/test_launcher_locale.py /ruta/a/wine-source
/ruta/a/INSTALL/SETUP.INS` compila el conversor real de Wine y el detector real
del launcher. Comprueba el fallback desde C, el instalador raíz y directo,
y el rechazo de otros nombres, rutas, tamaños y contenido alterado. El
archivo propietario del CD se proporciona externamente, no se incorpora al
repositorio. `make user-update` instala el launcher en una imagen detenida.

Validación en QEMU/KVM con el launcher final: arranque normal en-US, apertura
de SETUP.EXE de la raíz del CD, clic en Install y bienvenida de Quake II sin
el aviso de idioma. No se certificó todavía la instalación completa ni el
renderizado 3D del juego. La imagen de usuario recibió el launcher comprobado.


## Archivos grandes en BFS4: instalación de Quake II

El error de transferencia al copiar `pak0.pak` se correspondía con el límite
VFS de 64 MiB para la caché completa de archivos escribibles, no con el espacio
libre de la imagen. Se mantiene la caché para archivos pequeños; en el disco
nativo BFS4, los archivos mayores de 1 MiB pasan a lectura y escritura por
rangos. `bfs4_write_at` preserva los extremos de bloques parciales y agrupa
bloques contiguos. `bfs4_resize` permite truncar y extender sin cargar el archivo
completo; los huecos y las extensiones se rellenan con ceros.

La transición descarga la caché sucia, invalida las copias privadas y actualiza
los otros descriptores abiertos. Si falla una transferencia, se conserva el EOF
anterior y se liberan las nuevas reservas. Una escritura fallida dentro del
archivo puede haber modificado parte de sus datos, como cualquier E/S parcial.
Los mapas compartidos mantienen sus restricciones actuales. FAT heredado
conserva su ruta anterior; esta mejora se aplica al disco BFS4 de usuario.

El máximo práctico es 2147483647 bytes por archivo, porque seek, tell y size
usan enteros de 32 bits con signo. Las escrituras que lo superan devuelven
EFBIG; no se afirma soporte ilimitado ni se cambia el formato del disco.
El espacio libre del volumen sigue siendo necesario.

`python3 tests/desktop/test_large_files.py` compila las funciones reales del VFS
sobre el driver BFS4 real. Escribe y verifica 183997730 bytes (el tamaño del PAK
probado), limita la caché a 1 MiB, reabre para lectura/escritura y comprueba
aliases, escrituras parciales, huecos, truncado, crecimiento, ENOSPC, overflow y
rollback ante una falla de escritura de bloques inyectada. La prueba BFS4 host
existente también verifica append, extents indirectos, liberación y remontaje.

Validación QEMU/KVM adicional sobre una copia de la imagen: el instalador real
llega a la copia de `pak0.pak` con la nueva ruta. Se detuvo esa prueba antes de
completar la instalación; la verificación completa de los 183997730 bytes se
hizo con la integración VFS/BFS4 anterior, no con el instalador. La imagen de
usuario recibió el kernel final y se verificó su contenido binario.


## Quake II: conflicto al cargar el ejecutable y fases ATAPI

El ejecutable del CD tiene ImageBase 0x00400000, SizeOfImage 0x00701000 y
relocaciones eliminadas. Su rango 4..11 MiB chocaba con la BSS del kernel,
ubicada antes en 8..12 MiB; Wine devolvía STATUS_CONFLICTING_ADDRESSES
(c0000018). Se amplía la ventana PE a 4..32 MiB, se sitúa la BSS en 32..36 MiB
y comienza el heap en 36 MiB. Las protecciones de mmap siguen rechazando
solapamientos con el kernel y su heap: no se desactivan para forzar la carga.
El linker comprueba que la BSS no alcance el heap. Esto reserva 24 MiB físicos
adicionales y mantiene el requisito mínimo existente de 64 MiB de RAM.

La lectura READ(12) negociaba fases de un solo sector CD (2048 bytes), incluso
para peticiones de varios sectores. Ahora negocia hasta 32768 bytes por fase,
conservando el procesamiento del tamaño realmente informado por la unidad.
`python3 tests/desktop/test_atapi_ready.py` prueba el driver real con 1, 16, 32
y 255 sectores, tanto con fases de 32 KiB como con una unidad que solo entrega
2 KiB. La reducción de fases no implica una medición del tiempo total del
instalador, que también incluye Wine, gráficos y escritura del disco.

Validación QEMU/KVM (AHCI, 2 CPU, 1536 MiB): el kernel con BSS en 0x02000000
arranca y Wine queda READY en 9123 ms en esta copia. Con los archivos originales
del CD en la carpeta de prueba, QUAKE2.EXE supera la carga del módulo principal,
no devuelve c0000018 y crea su ventana. La ventana permanece negra; no se
certifica todavía menú, renderizado 3D ni estabilidad al cerrar el instalador.
La prueba ATAPI con sanitizers pasó. La imagen original de usuario se actualizó
con QEMU cerrado, conservando la instalación del usuario, y se comprobó que el
kernel instalado coincide byte por byte con el compilado.


## Quake II: commits de memoria y salida de video por software

La ventana vacía no demostraba por sí sola una falla de la GPU. Con registro
del juego y Wine habilitado en la copia de prueba, el renderizador ref_soft
inicializaba correctamente, pero la carga del mapa terminaba con
“VirtualAlloc commit failed. Access denied.”. Se comprobó además un mprotect
fallido sobre varias vistas ELF adyacentes. El kernel solo admitía un rango
contenido en un VMA respaldado o cubierto exclusivamente por reservas vacías.

`sys_vm_mprotect` ahora precomprueba la cobertura y permisos de archivos
compartidos, y procesa los tramos de cada VMA cuando un rango cruza varios.
Esto permite habilitar un prefijo ya respaldado junto a su cola reservada,
sin reemplazar ni poner a cero los datos existentes. También permite cambiar
protecciones de varios tramos ELF contiguos. Las restricciones sobre kernel,
heap, huecos y archivos compartidos de solo lectura permanecen activas. Si
falla una operación de un tramo, se devuelve el error; no se declara atomicidad
para cambios de protección sobre varios tramos.

Se amplió CASE=7 de `ports/wine/tests/runtime.c` con commits incrementales de
tres páginas, conservación de bytes, cero de páginas nuevas y alternancia
lectura/escritura entre varios VMAs. Se actualizaron las direcciones de prueba
de BSS/heap a su ubicación actual. El ejecutable Linux i386 compilado se probó
en el kernel real de QEMU: CASE=7 PASS y status=0, incluyendo protecciones del
kernel, DOS, hints y rechazos de escritura mediante getrandom.

QEMU/KVM, AHCI, 2 CPU, 1536 MiB y SVGA: Quake II mostró el mapa 3D y HUD usando
ref_soft en ventana de 640x480; la escena avanzó entre capturas y Escape abrió
el menú. La prueba no certifica OpenGL ni aceleración 3D. Los logs y el launcher
de diagnóstico son exclusivos de la copia temporal y no se distribuyen en
la imagen del usuario.


El default.cfg incluido en el PAK selecciona vid_fullscreen=1. Esa ruta inicial
DirectDraw queda sin imagen en la configuración virtual probada. Para el
primer arranque se agregan `+set vid_fullscreen 0 +set vid_ref soft` únicamente
si el destino es QUAKE2.EXE, hay un baseq2/pak0.pak con firma PACK y no existen
config.cfg ni autoexec.cfg. No se modifica ningún archivo de configuración.
Una configuración existente tiene precedencia; el usuario puede cambiar luego
sus opciones de video dentro del juego. El detector se prueba con
`python3 tests/desktop/test_quake_first_video.py`, incluyendo archivos guardados,
otros nombres, PAK inválidos y rutas largas. Esto permite arrancar en ventana;
no declara corregida la ruta DirectDraw de pantalla completa.

Validación final con el launcher de producción y sin config.cfg/autoexec.cfg:
el primer arranque dibuja la demo 3D y HUD en ventana (320x240, modo original
del juego); se observó avance de la escena entre capturas. El kernel y launcher
finales se instalaron y verificaron en la imagen original, conservando los
archivos instalados y sin incluir el ejecutable de prueba VMTEST.ELF.

## Quake II: OpenGL y fragmentos de audio AC97

Al activar ref_gl, la carga de texturas ampliaba el heap de Wine con commits
incrementales. El mprotect de cada prefijo recorría repetidamente toda la tabla
de VMAs dentro de llamadas recursivas. Ahora la cobertura se calcula con una
pasada sobre las intersecciones de VMAs no superpuestos y los tramos respaldados
se protegen directamente; solo las reservas vacías pasan por la ruta de commit.
Se conservan los backings, los permisos COW y la precomprobación de cobertura.

Además, paging_process_access_mode compara los permisos con las PTEs reales.
Si no cambian, no publica una generación TLB ni espera confirmaciones de otras
CPU. Los cambios reales de lectura/escritura, PROT_NONE y COW siguen invalidando.
Esto evita repetir sincronizaciones cuando Wine recommite páginas existentes.
`tests/desktop/test_vm_protect.py` comprueba ambos caminos y el rechazo de huecos.
CASE=7 se amplió a 128 commits sucesivos, conservando bytes anteriores y cero de
páginas nuevas. El binario i386 pasó en el kernel real con dos CPU (status=0).

Validación gráfica: QEMU/KVM, AHCI, 1536 MiB, dos CPU, -vga vmware, config.cfg
con vid_ref=gl, vid_fullscreen=0 y gl_mode=3. La demo llega al mapa, muestra
texturas y HUD, avanza entre capturas y Escape abre el menú. Mesa informa
renderizado por CPU: esta configuración no negocia SVGA3D. No se certifican
aceleración GPU, pantalla completa ni rendimiento comparable a ref_soft.

El DMA de AC97 tenía otra causa de audio entrecortado: el último descriptor
parcial conservaba el tamaño de 2048 frames y reproducía también el relleno de
ceros. Ahora su longitud describe solo los frames renderizados, en muestras
estéreo para ICH o bytes para SiS. Un bloque de 4800 frames deja de ocupar 6144.
`tests/desktop/test_ac97_fragments.py` verifica longitud parcial, reutilización
del anillo y unidades SiS; el test OSS existente también pasa. Se capturó PCM
mediante QEMU WAV, pero la calidad audible en la salida del usuario requiere
su prueba. Los logs habituales de audio permanecen desactivados.
