# Wineboot: investigación desde W3-67 (2026-09-19)

Objetivo original: `wineboot --init` debía terminar con estado 0 dentro de
BlesKernOS y crear realmente el prefijo. El resultado aprobado y sus límites
figuran al final de esta bitácora. Un timeout, avanzar en módulos o terminar
sólo el launcher no constituyen aprobación.

## Punto de partida comprobado

- No hay repositorio git en este árbol. Copias de los archivos previos a esta
  sesión: `build/w3-68-original/` (syscall.c, fat.c, run-runtime.py,
  wineboot-launch.c).
- W3-66, W3-66b y W3-67 ya tienen resultados en `build/`; W3-67 termina en
  -998 a los 733 s. No hay QEMU previo vivo al comenzar.
- `boot event wait timed out` viene de `ntdll/unix/env.c:run_wineboot()`:
  espera el bootstrap completo durante cinco minutos; no identifica por sí
  solo un bloqueo en services/SetupAPI.
- Fuentes Wine 11.0 de ntdll, wineboot y services extraídas del tarball local
  fijado a `build/wine-source/wine-wine-11.0/` para contrastar el protocolo.
- W3-68 (1 CPU, 768 MiB, código original) se interrumpió deliberadamente
  después de 129 s para comparar el coste de las búsquedas FAT. ABI y case30
  pasaron; wineserver seguía inicializando. No es una prueba completa ni un
  timeout del guest.

## Cambios en evaluación

- `find_directory_chain_entry`: ceder cada 64 bloques recorridos en vez de
  cada cluster. La normalización comprueba enlaces y modos para cada
  componente y multiplicaba las cesiones incluso en directorios pequeños.
- Caché de sectores FAT: spinlock que protege tags, datos y generación;
  elimina la desactivación del caché con SMP. No realiza I/O con el lock.
- Crear o reescribir archivos vacíos sin cadena evita leer toda la FAT;
  truncar archivos con cadena conserva el camino transaccional completo.
- Opción `--profile-syscalls` del runner: compilación diagnóstica que registra
  llamadas con duración >=10 ticks. No habilitada en builds normales.
- Launcher: timestamps y cargas de DLL, errores y trazas service/RPC en lugar
  de advertencias de cada búsqueda fallida de archivo.

## Pruebas

- W3-69 en `build/w3-69-fat-lookup`: 1 CPU, 768 MiB, case30 + wineboot,
  profile-syscalls. Incluye sólo el cambio de frecuencia de yield FAT (no
  los cambios posteriores de caché SMP, archivos vacíos ni WINEDEBUG).
  Compiló y ABI/case30 pasaron. Se interrumpió deliberadamente a los 305 s
  tras identificar llamadas fstatfs64 (nr269) de 3–9 segundos repetidas
  durante las búsquedas de DLL. Este resultado no aprueba W3.
- Build del caché SMP: `build/w3-70-cache-build.log`, compilación exitosa.
  Aún falta la validación en guest y de las optimizaciones posteriores.

Siguiente prueba: kernel actualizado con 2 CPU; cases25 y30 (filesystem y
symlinks), wineboot y profile-syscalls. Ejecutar un solo QEMU a la vez.

## W3-70: causa medida y siguiente ejecución

El recorrido de toda la FAT en `fat_get_space` se repetía en cada fstatfs64
(para consultar tipo de filesystem). Se añadió caché de un conteo exacto,
actualizado desde la tabla en cada commit completo o parcial y invalidado
ante errores, formato y cambio de dispositivo. Un miss lee sectores por lotes.
Se conserva una generación bajo spinlock para evitar publicar resultados
atravesados por otra invalidación. No se inventan valores de espacio libre.

`filesystem.c` / case25 ahora comprueba statfs antes/después de reservar 16 KiB,
truncar y borrar. El binario case25 del rootfs y su hash fueron actualizados
sin sustituir glibc ni otros binarios.

W3-70 está en `build/w3-70-statfs-cache`, 2 CPU, 768 MiB, cases25,30 y wineboot,
profile-syscalls. Incluye todas las modificaciones de esta sesión. Comprobar
resultado y logs; aún no hay aprobación de wineboot.

W3-70: case25 (incluyendo conteo libre) y case30 aprobados. Services carga
SetupAPI y llega al servidor RPC; luego wineboot lanza rundll32 (381 s).
El padre agota su espera interna a los 398 s; se interrumpe deliberadamente
la prueba a unos 460 s para probar la siguiente mejora. No hubo fallos de
memoria registrados. Sigue sin aprobar W3.

W3-71: caché de resultados positivos de `fat_resolve_path` (32 entradas),
protegido con el spinlock y generación del caché de sectores; cualquier
escritura invalida los metadatos. No se cachean errores. También se invalidan
sectores tras errores de escritura parcial. Se conservó el código de FAT
de W3-70 en `build/w3-70-statfs-cache/fat-source.c`.

El runner agrega `--wineboot-full-runtime`: stagea lib/wine y share/wine del
paquete fijado, necesario para los winedevice/mountmgr de PreInstall y las
DLL/datos que pide DefaultInstall. Requiere imagen >=512 MiB.

W3-71 se ejecuta en `build/w3-71-full-prefix` con 768 MiB de disco, 768 MiB RAM,
2 CPU, cases25,30, runtime completo y profiler. Falta comprobar su resultado.

W3-71: case25/30 aprobaron. Services a 121 s y rundll32 a 183 s. Luego
aparecieron STATUS_TOO_MANY_OPENED_FILES (c000011f) al iniciar explorer y
fallos al obtener objetos compartidos de sesión. Sólo 18 handles FAT abiertos
cuando comenzaron los errores: investigar límite global de IPC (32 pares),
no asumir saturación de los 128 handles VFS. Otra causa medida: getdents64
sobre 820 DLL demoró 66 s, pues calculaba cada inode mediante búsquedas
completas de ruta/directorio. Se interrumpió la ejecución para validar fixes;
no pasó wineboot y hubo timeout interno del padre.

W3-72 (`build/w3-72-ipc-directory`) incluye:
- Snapshot de la tabla FAT confirmada para transacciones pequeñas, invalidado
  por generación; copia privada por transacción, commits síncronos. Cache
  opcional <=4 MiB y sólo si RAM total >=64 MiB.
- Espera ATA DRQ con timeout temporal y fallback temprano; ignora ERR/DF
  mientras BSY está activo, evitando resets por un poll-count demasiado corto.
- 128 pares IPC en vez de 32. El rango de dispositivos se desplaza después de
  los endpoints; GC de SCM_RIGHTS recorre todos los endpoints.
- getdents64 obtiene posiciones e identidad en una sola enumeración FAT,
  reutiliza el caché por descriptor y devuelve tipo/inode del symlink en sí.
  No cambia el layout público de fat_dir_entry_t: usa metadata separada.
  Rewind invalida el snapshot del directorio.
- case23 prueba 56 pipes simultáneos, separación con /dev/null y 50 ciclos
  SCM_RIGHTS en endpoints altos. case25 prueba inode/tipo de symlink enumerado
  además de statfs. Hashes del rootfs actualizados.
- Runtime full también incluye las dependencias ELF disponibles en el paquete
  (sin reemplazar la libc/ld del rootfs). Trazas de wineboot activadas.

Comando W3-72: cases23,24,25,30; --wineboot --wineboot-full-runtime
--wineboot-image-mib 768 --cpus 2 --ram-mib 768 --seconds 1300
--case-seconds 1200 --profile-syscalls. Resultado pendiente.

Antes de aprobar: probar launcher sin las variables históricas de diagnóstico
WINEDLLDIR0/WINEBOOTSTRAPMODE, comprobar salida real 0 y prefix/registro.

W3-72: ABI y cases23/24/25/30 aprobaron. Rundll32 lanzado a 331 s;
timeout interno del padre a 350 s. La prueba de getdents de case25 pasó; falta medir el directorio completo. Se detuvo
deliberadamente después de identificar dos frees inválidos de vm.maps
(caller 0x12d8fa en kernel-test.elf): clone asignaba la tabla del hijo al
owner del padre, y mm_release_process del padre la liberaba prematuramente.

W3-73 fija owner 0 para vm.maps (cleanup explícito) y cachés de descriptores
VFS que pueden heredarse, además del snapshot FAT global. Case21 agrega
un nieto que usa su VMA y cache heredado después de reapear su padre.
fat_read_file_at evita leer contenidos de clusters anteriores al offset.
Profiler identifica fd/raw/path de reads/writes lentos. Próxima ejecución:
cases21,22,23,24,25,28,30 + wineboot full, 2 CPU, 768 MiB RAM/disco.

W3-73: cases21/22/23/24/25/28/30 aprobaron, incluyendo el nieto
que sobrevive a su padre. Sin frees inválidos. Rundll32 a 152 s (antes331).
Falla find_shared_session_object en win32u desde215 s. Fuente Wine confirma
que vistas NT readonly usan MAP_PRIVATE en Linux y esperan ver cambios del
archivo; el kernel actualmente copia esas vistas al mapearlas. Investigar
COW de vistas privadas de objetos ya compartidos; habilitar warn+winstation.

W3-74 agrega lecturas pequeñas sin precargar toda DLL (read7 repetido
medido en .dll.so) y case25 para offsets/lejos/EOF. Runner rechaza además
free inválido y corrupción de heap. Se conserva diagnóstico de bootstrap.

W3-74 confirmó en warn+winstation: `Session object id doesn't match
expected id 1/2/3`. Case25 (lecturas pequeñas, offsets y EOF) y28 pasaron.
Se detuvo para probar la corrección de coherencia.

W3-75: MAP_PRIVATE de un archivo que ya tiene backing compartido conserva
esas páginas hasta la primera escritura privada. PTE software COW, permiso
por página y materialización de toda la vista al primer write; mprotect,
fork y copy_to_user conservan la semántica. Los archivos sin backing
compartido mantienen el mecanismo previo de copia (no es un page cache
general). VFS mantiene cachés de archivos abiertos al unlink y registra
que están borrados, incluyendo mmap posterior; el flush no recrea nombres.
Case28 prueba cambios visibles en vista privada readonly, aislamiento al
escribir, permisos de otra página y escritura vía pread/copy_to_user; también
unlink antes de mmap y ausencia de resurrección del nombre tras cerrar.
Prueba aislada en build/w3-75-cow-regression (21,22,25,28) en curso.

W3-75 aprobado: ABI,21,22,25,28, sin faults fatales ni errores de heap.
La falta de permiso en segunda página produce SIGSEGV esperado y manejado.
Prueba completa W3-76 en build/w3-76-session-cow, case28 + wineboot full,
2 CPU/768 MiB; conserva variables diagnósticas para comprobar causalidad.

W3-76 termina con wineboot-launch status256 (~468s), no aprobado.
Desaparece el fallo de IDs de sesión; PreInstall logra CreateService(MountMgr),
pero el handle devuelto coincide con el manager y RPC lo rechaza (tipo1/2).
Además pthread_exit aborta porque falta libgcc_s.so.1. La fuente getrandom
reinicializaba cada word con tick/PID/stack: un buffer completo repetía la
misma word, produciendo colisiones en UUID almacenados en caché por Wine.
W3-77 agrega estado global protegido y contador/TSC al fallback sin RDRAND.
Esto corrige repetición; la calidad criptográfica de la entropía temporal
sigue siendo una limitación del kernel, no una CSPRNG certificada. Case18
verifica128 bloques de16bytes distintos dentro de una sola syscall.
Rootfs incluye libgcc_s y FreeType+dependencias i386 del mismo host (hashes
en manifest); build-runtime conserva esa lista para reconstrucción.

W3-77:18/28 pasaron; wineserver quedó sin avances tras avisos de TLB.
Interrumpido para repetir con monitor Unix QEMU (ahora el runner lo habilita).
W3-78: arranca wineboot normalmente, no reproduce ese bloqueo temprano.
Case06 agrega pthread_exit+cleanup y falló por RUNPATH no heredable de libc
al cargar libgcc_s: el test se recompiló con RPATH heredable; Wine usa
LD_LIBRARY_PATH explícito y sí encontrará la dependencia en esta ejecución.
Falta repetir case06 corregido en la regresión final. W3-78 en curso.

W3-78 interrumpido: FreeType carga pero Fontconfig faltaba. El NULL call
se localizó con addr2line/objdump en win32u.so:create_family_pattern,
llamada pFcPatternCreate. Se añadieron libfontconfig/libexpat i386 y
fonts.conf apuntando a /OPT/WINE/share/wine/fonts, con caché en WINEGATE.
Prueba siguiente W3-79: case06 con RPATH corregido,18,28 y wineboot full.

W3-79: ABI,06 (pthread_exit+cleanup),18,28 pasan. Fontconfig elimina
el NULL call anterior; el escaneo de fuentes consume varios minutos y
el padre vence su espera interna de300s. Interrumpido para W3-80:
cache positiva de ubicaciones FAT (permisos/inodo/LFN), invalidada por
cada escritura. Pruebas25/30 + wineboot full. Se borraron sólo imágenes
desechables71/72/73/74/77/78 para recuperar espacio; logs conservados.

W3-80: ABI25/30 pasan, luego bloqueo temprano reproducido. HMP encuentra
CPU0 en kmutex_lock(IF=0), mutex g_domains[GUI] con owner1 (GUI lista
en esa CPU), Wine pid17 esperando al restaurar dominios en task_yield.
El camino sti;hlt;cli restauraba mutex antes de IF, impidiendo ejecutar
al dueño. W3-81 restaura IF original antes de los mutex en ambos caminos
SMP de task_yield; kernel_switch_safe se desactiva antes como antes.
Prueba21/23/25/28/30 + wineboot full.

W3-81: ABI21/23/25/28/30 pasan, llega a services/rundll32 sin deadlock.
Muestreo HMP:31/40 muestras de CPU activa en ATA. Interrumpido para
W3-82 con cache FAT de4 vias/256 sectores (128KiB), dinámica sólo con
RAM>=64MiB, owner0; conserva32 sectores estáticos para modo4MiB.
El primer intento estático excedía límite BSS y fue sustituido por reserva
dinámica; layout HighMem aprobado (BSS fin0x3357c8).
W3-82 usa entorno normal sin WINEDLLDIR0/WINEBOOTSTRAPMODE: ya carga
wineboot/services. Se recompilaron30 casos y dependencias con build-runtime.
Launcher fuente para próxima prueba agrega wait del wineboot real, consultas
reg.exe a MountMgr/ImagePath y CurrentVersion/ProductName, wineserver-k15/-w
y chequeo de los tres hives persistidos. W3-82 aún usa launcher anterior.

W3-82:25/30 pasan y entorno normal arranca services/rundll32/explorer.
El recorrido de fuentes sigue lento; muestreo:54/80 muestras en HLT de
task_yield, IPC de64bytes tarda100-500ms con peers listos. Interrumpido
tras~350s para W3-83: int80 originado en kernel usa ABI nativa aunque
la tarea sea Linux; task_yield Linux Ring0 permite reschedule voluntario
inmediato con kernel_switch_safe, como los workers nativos. No se cambia
la numeración ni los registros de syscalls Linux Ring3. Batería completa
ABI+30 casos y wineboot con nuevo launcher de verificación en W3-83.

W3-83: ABI y los30 casos pasan (incluyen modificaciones06/18/21/23/25/28).
Wineboot continúa con entorno normal. Nueva raíz recompilada conserva
exactamente hashes libc/ld de W3-79. Resultado final aún pendiente.

Pendiente siguiente imagen: Fontconfig apunta a /WINEGATE/share/fonts
(fuentes host), conservando todas las fuentes builtin. Fuente upstream
win32u/font.c:load_file_system_fonts ya recorre share/wine/fonts; incluir
el mismo directorio en Fontconfig provocaba escaneos duplicados en cada
proceso. Nuevo directorio host vacío se incluye en rootfs, config/manifest
actualizados. No se retiran fuentes ni se modifica Wine.

W3-83: RPC crea MountMgr y devuelve handle diferente del manager; se
confirma la corrección RNG. StartService falla8: syscall clone120 devuelve
ENOSYS cuando services la invoca desde tid130/proc115 (thread RPC).
task_linux_fork rechazaba parent->thread. W3-84 permite fork desde worker:
TLS, señales/frame proceden del llamador; stack original del proceso desde
el líder, fijando allocation antes de copiar; stacks pthread se clonan
como VMAs. Se mantiene restricción previa de vfork multithread. Case21
nuevo: fork en pthread, ambos stacks/TLS privados, nieto y exec ABI.
W3-83 terminó naturalmente: timeout interno427s, wineboot status13568
(exit53), launcher256; no faults fatales. Siguiente84 usa Fontconfig sin duplicar fuentes builtin
y cases06/21/22/23/24/28/29 + wineboot verificado.

W3-84: ABI,06/21/22/23/24/28/29 pasan; case21 confirma fork desde
worker con TLS, stack principal y pthread, nieto y exec. Wine en curso.
Se eliminaron imágenes desechables79/80/81/82 conservando sus logs.

W3-84: fork RPC funciona y winedevice.exe arranca. Services lo termina
al vencer service_pipe_timeout upstream de10s; el log muestra que seguía
cargando DLL (ntoskrnl/cryptbase), no una excepción del proceso. PreInstall
continúa pero el padre vence300s (362Wine), sale53; launcher256.
W3-85 amplía cache de lectura de acuerdo con RAM (16MiB>=512MiB,
4MiB>=128MiB,1MiB>=64MiB;32sectores en bajo consumo) para retener
DLL comunes entre procesos. Invalidar una escritura busca sólo sus sets,
no recorre miles de slots. Tamaño BSS validado. No se cambian timeouts de
Wine. Siguiente prueba25/30 + wineboot verificado.

W3-85: ABI25/30 pasan. Reduce tiempo hasta CreateService de305 a209s.
Luego sin salida nueva: HMP confirma ambas CPUs en isr8 (#DF), CR3 de
services, última reserva0x110000..0x210000. sys_vm_mprotect promovía
reservas virtuales sin el guard de kernel bajo que sí tenía MAP_FIXED.
W3-86 aplica protección de0x110000..0x400000 también a reservas (EEXIST
para NOREPLACE) y a mprotect (ENOMEM), conservando DOS bajo0x110000.
Case07 agrega reserva+commit/protect de esa ventana: deben fallar sin
alterar el kernel. Tests07/21/25/28/30 + wineboot full en próxima86.
package-wine.py ahora incluye semillas dlopen(libgcc_s/FreeType/Fontconfig)
y su cierre ELF, además de fonts.conf/dirs host; syntax-check aprobado.
Todavía no se regeneró el archivo tar del paquete.


## Retoma 23 de septiembre: W3-87 / W3-88

Se reconstruyo el kernel desde esta copia (no habia build principal). W3-87
usa full03/test.img como fuente aislada, runtime recompilado con hashes de
libc/ld iguales a rootfs, Wine completo, 768 MiB y dos CPUs. ABI y 07/25/30
pasan; MountMgr se crea a ~153 s. No reproduce #DF, pero el heap aparece
corrupto bajo el CR3 de services al iniciar otro hilo (~157 s).

HMP, guest detenido: CPU0 CR3=19e36000 ve en 0x800000
b1e5c0de,20,0,0,800038,0; xp (memoria fisica) muestra esos mismos datos.
CPU1 CR3=1d678000 ve seis ceros en la direccion virtual 0x800000.
Esto confirma sustitucion del alias, no destruccion de la cabecera fisica.
W3-87 se detuvo por HMP quit, result.json queda no aprobado con corrupcion.

W3-88 extiende el rechazo de MAP_FIXED/MAP_FIXED_NOREPLACE (incluidas
reservas PROT_NONE) al heap y pool fisico de paginas. El allocator expone
la comprobacion de rangos y conserva el hueco ET_EXEC, salvo su pagina de
metadata. mprotect en identidades solo acepta rangos propiedad del cargador
ELF (RELRO de PIE/ld.so); el guard bajo de W3-86 sigue vigente. Se corrige
el comentario incorrecto que consideraba seguro sustituir el alias del heap.
Case07 cubre raiz del heap, rango que cruza su inicio, otra zona del heap,
MAP_FIXED respaldado y reservas, mprotect, hint reubicable y ventana DOS.
Resultado de la siguiente ejecucion pendiente; no se declara W3 aprobado.

W3-88 final: ABI y07/06/21/22/23/24/25/28/29/30 pasan; no hay corrupcion
ni faults fatales. MountMgr se crea (~155s), winedevice entra pero vence
la espera de10s mientras carga ucrtbase/sechost/advapi32. A343s vence la
espera del padre, exit53; launcher256. result.json no aprobado,363.6s.

W3-89 prueba cache FAT de64MiB con>=512MiB de RAM (antes16MiB): el
conjunto compartido incluye shell32 de9MiB y numerosas DLL y se expulsaba
antes de iniciar winedevice. Los cursores de reemplazo ahora se reservan
con la tabla, bajando BSS8KiB y conservando32sectores en modo4MiB.
Se activa el perfil de syscalls lentas. El launcher invoca explicitamente
wine C:\windows\system32\wineboot.exe --init, valida prefijo inicialmente
ausente y añade un marcador de registro para probar reinicio real.
Nuevo verify-prefix.py copia solo una imagen aprobada, preserva kernel/FAT,
reemplaza el driver de prueba y verifica el marcador sin volver a crearlo.
No se afirma persistencia hasta que este runner pase. Ambos modos del
launcher compilan con -Wall -Wextra sin avisos; el verificador rechaza W3-87.

W3-89: ABI07/25/30 pasan; sin faults fatales ni corrupcion. Winedevice
responde al dispatcher dentro del plazo (~151..156s). MountMgr carga su
.sys y publica SERVICE_RUNNING/error0 a159.07s. Primer arranque real del
servicio confirmado. La instalacion completa sigue creando directorios y
vence el timeout de300s del padre (339.38s de reloj guest), exit53;
launcher256, duracion356.9s. No se afirma wineboot ni persistencia aprobados.

W3-90: el perfil muestra mkdir/open/close pequeños costosos durante INF.
fat_commit_dirty_table contaba todos los clusters incluso cuando cambian
uno o dos sectores. Se deriva el delta libre desde el snapshot confirmado,
validando volumen/generacion y conservando full scan si falta snapshot o en
FAT12/16. Case25 verifica espacio libre exacto despues de asignar16KiB,
reescribir, truncar8KiB y truncar0. Se prueba tambien con1CPU para separar
el coste SMP. Kernel compilado y layout HighMem aprobado; resultado pendiente.

W3-90 final: ABI y07/25/28/30 pasan, MountMgr RUNNING a109.05s con1CPU.
Sin corrupcion ni faults fatales. El padre agota300s, exit53 (launcher256).
Los tres hives y250 entradas de system32 existen, pero la instalacion esta
incompleta; no se aprueba ni persistencia ni wineboot.

W3-91 reduce directorios nuevos de32 clusters a1 y prueba crecimiento con
48 nombres largos, listado y borrado. Creacion/listado/unlink pasan; case25
falla al limpiar el directorio porque rmdir(40) no estaba implementado.
ABI07/30 pasan, MountMgr RUNNING114.27s. Wine vuelve a agotar300s, exit53;
launcher256,335.4s, sin faults fatales. Se mantiene la prueba fallida visible.

W3-92 conecta rmdir con controles de tipo, raiz, punto y directorio no vacio,
y verifica errores ENOTEMPTY/ENOTDIR/ENOENT/EINVAL ademas del borrado real.
Agrupa LFN y entrada corta por sector, conservando la publicacion de la entrada
corta al final y la metadata POSIX de archivos reescritos. Tambien usa esta
rutina en rename. Muestreo HMP de91 encontro9/50 PCs en ata_wait_not_busy:
cada fragmento LFN antes provocaba WRITE+FLUSH individual. Resultado pendiente.
Se corrige el marcador esperado por verify-prefix.py para coincidir con el
launcher de reutilizacion. No se ha ejecutado aun un reinicio aprobado.

W3-93 preparado: busqueda FAT por sectores con buffer de512B en pila, sin
kmalloc/free de cluster en cada componente; conserva estado LFN entre
sectores/clusters y soporte FAT12/16. Esperas IPC/poll/select/connect/accept:
16 handoffs inmediatos y luego sleep1tick por intento sin datos; nunca duerme
con el dominio VFS/IPC retenido. Es un backoff de la espera por sondeo, no una
cola de eventos nueva. Se validara con ABI+los30casos y Wine completo antes
de conservarlo como solucion. Riesgo medido: hasta1tick adicional cuando un
peer responde despues de los primeros16intentos. Build/layout/reserva pasan.

W3-92 final: ABI07/25/30 pasan (incluidos crecimiento y rmdir). MountMgr
RUNNING124.60s; sin faults fatales. Se agota espera300s a327.18s, exit53,
launcher256;342.12s total. La agrupacion LFN conserva correccion, pero no
resuelve por si sola el tiempo de instalacion. W3-93 ejecuta los30casos.

W3-93: ABI ylos30casos terminan status0 antes de lanzar Wine; integracion
sigue en curso. Se añaden a case23 pruebas nuevas con peers demorados100ms:
read/poll/select, EOF, timeout30ms y read interrumpido por SIGUSR1/EINTR.
Estas pruebas nuevas se compilaron despues del staging de93 y requieren otra
pasada: no se cuentan entre los resultados de93.

W3-93 final: ABI30/30 pasan; MountMgr RUNNING119.35s, sin faults fatales.
Wine sigue agotando300s y devuelve53, launcher256;363.97s incluyendo todos
los casos. Perfil HMP de100muestras durante copia:15en kmemset, multiples
en parse/copy_name/lookup FAT,6ATA busy y7task_yield. Se sigue investigando.

W3-94: fat_lfn_reset deja de limpiar260B por cada entrada corta; solo borra
el buffer al iniciar una secuencia LFN. Lookup compara primero el nombre y
materializa fat_dir_entry_t solo para la coincidencia; listado sigue completo.
El cierre de referencia temporal IPC omite GC si quedan referencias externas:
no puede desaparecer una raiz del grafo en ese caso. Se mantienen GC cuando
refs==inflight y las pruebas existentes de ciclos SCM_RIGHTS. ABI+30casos,
incluidas las nuevas esperas demoradas de23, yWine en ejecucion.

W3-94 final: ABI30/30 pasan, incluidas nuevas esperas demoradas/EINTR yGC.
MountMgr RUNNING97.32s; sin faults fatales. Wine agota300s y sale53,
launcher256,348.46s total. system32 contiene509entradas al detener QEMU,
hastaSCCBASE.DLL: mas avance que las250 de90, pero instalacion incompleta.

Se comprueba /dev/kvm accesible y API12. Runner admite --accel tcg|kvm,
defaultTCG sin cambios. W3-95 prueba el mismo codigo con KVM,1CPU,768MiB,
ABI+30casos yWine completo, sin cambiar los plazos de Wine ni el reloj guest.
El perfil opcional ahora mide lookup/asignacion/datos/publicacion de FAT;
solo se compila en el objeto separado fat-profile.o del harness. Esto permite
separar errores funcionales del coste TCG. Aun no hay resultado de95.

W3-95 KVM: ABI y01..22 pasan. La prueba nueva de23 tiene una carrera propia:
el hijo enviaSIGUSR1 mientras el padre aun retorna de fork (trace muestra
SIGDELIVER en el retorno de fork); luego el padre hace read sin otro escritor.
Se detiene QEMU porHMP; resultado no aprobado, Wine no llego a ejecutarse.
Se corrige el test con pipe de sincronizacion antes de enviar la señal y un
byte de rescate posterior para que una interrupcion fallida no cuelgue el gate.
W3-96 repite ABI30/30 yWine bajoKVM con esa prueba corregida.

W3-96 KVM: ABI y30/30 pasan, incluido23 corregido. Perfil de25: escribir
131072B dedica192ticks a datos (~1.92s), lookup0/alloc4/publish0. IDEPIO bajo
KVM resulta mas lento aqui; se interrumpe la ejecucion de Wine temprano y el
resultado global queda no aprobado. No se declara wineboot probado enKVM.
W3-97 vuelve aTCG con perfil FAT y casos07/23/25/30. Kernel funcional igual
al de94; cambian solo instrumentacion opcional y sincronizacion del test23.

Cambio de alcance autorizado por el usuario: ampliar la espera de Wine y/o
mejorar el controlador de disco. Se prioriza la espera15min para poder validar
la instalacion completa sin seguir chocando con300s. Parche versionado de una
linea funcional enntdll/unix/env.c; build-wine.py aplica y registra hashes de
la lista deparches fijada enversions/wine-11.0.json. Fuente tar verificada.
Configure original falla por headersX11 ausentes en este host. Se reconfigura
solo la construccion de ntdll.so sinX/FreeType/Fontconfig; no se reemplazan los
modulos graficos existentes. Build especifico en/home/lucas/blesk-wine-w3-bootwait,
manifest-ntdll.json ybuild-ntdll.log guardan alcance ycomandos. El runner admite
--wine-ntdll para probar la nueva biblioteca sin modificar aun el paquete.

W3-97: ABI07/23/25/30 pasan, sin faults fatales. Timeout300s a323.46,
exit53/launcher256; compilacion de ntdll concurrente durante esta pasada,
por lo que no se comparan sus tiempos absolutos con94. Perfil FAT separa
costes: ejemplos de archivos pequeños alloc7..25ticks/datos0..6; tmpmap1.3MiB
usa433ticks en datos. Driver no es el unico coste, peroPIO sigue siendo caro.

ntdll.so reconstruido con espera15min: exports dinamicos iguales al modulo
anterior, ELF32, SONAMEntdll.so, NEEDEDsolo libc.so.6. RUNPATH/WINEGATE/lib
puesto al relink (patchelf no esta instalado). Artefacto yproveniencia en
build/bootwait-ntdll. SHA256 f878ec7b82f626b374a04d90d81dbe9f15efe94e6575cb302839ca91f5763414.
W3-98 prueba prefijo ausente, mismo kernel ysolo este modulo Wine sustituido.
Plazo del harness1800s por caso, Wine900s; resultado pendiente.

W3-98 final: ABI y07/23/25/30 pasan, sin faults fatales. MountMgr RUNNING
90.11s. La espera de900s permite copiar DLLs e idiomas, escribir ~864KB de
registro y lanzar control.exe, pero se agota a926.94s. Wine sale53, launcher256;
940.88s total. No hay inicializacion ni persistencia aprobadas.

Siguiente candidato: margen1800s y reconocimiento de MSDOS_SUPER_MAGIC como
case-insensitive en ntdll. El kernel ya compara nombres FAT sin distinguir
mayusculas ASCII y devuelve0x4d44 en statfs; Wine solo reconocia EXT4_CASEFOLD
yCIOPFS en Linux, recorriendo directorios tras búsquedas fallidas. Se conserva
el manejo de nombres cortos de Wine. Parche separado y fuente versionada.

W3-99 en curso: ABI07/23/25/30 pasan. Copia c_1253.nls a560.43s frente
a669.62s en98 (109.19s antes). Observacion parcial, no tiempo total ni gate
aprobado. Ambos parches reproducen exactamente env.c/file.c compilados al
aplicarlos a la fuente tar verificada. El modulo conserva ELF32/RUNPATH.
Se añade package-validated-runtime.py para actualizar un paquete base solo
cuando los gates inicial y de reinicio pasan y sus hashes coinciden. Prueba
negativa: rechaza W3-98 antes de crear salida. No se ha generado paquete nuevo.

W3-99 final: no agota1800s; falla a980.60s,994.25s total. Wineboot automatico
crashea al descargar wldap32.dll.so (destructor llama WSACleanup antes de
resolver su import), y mpr.dll falla con c000011f=STATUS_TOO_MANY_OPENED_FILES.
Wineserver habia llegado aFD127. Launcher sale256; no gate aprobado. Un
proceso winedevice anterior tambien informa errorfutex durante salida: se
añade diagnostico de errores futex inesperados para identificar errno/operacion.

Correccion candidata: tabla por proceso512FD, independiente deVFS_MAX_OPEN_FILES128
(pipes/socket/dup tambien consumenFD); select dimensiona sus conjuntos por ese
limite. Tabla asignada por proceso yowner0, liberada al limpiar elproceso; se
actualiza copia de flags en fork. Reserva estatica512 no cabia enBSS4MiB,
por eso se usa asignacion por proceso. BSSfinal0x330788, tests de layout/reserva
pasan. Case23 añade200dup simultaneos yF_DUPFD_CLOEXEC>=300 conpoll/select/read.
W3-100 valida ABI30casos,2CPU, antes de repetirWine.

W3-100: ABI30/30 pasan con2CPU en65.18s; prueba nueva200FD ypoll/selectFD300
pasa. W3-100-boot4m: GUI_RUN con4MiB, heaplibre38KiB. W3-101 repite prefijo
vacio con kernel512FD yntdll30min/FAT, casos07/23/25/30 antes deWine.

Auditoria de estado 2026-09-26: W3-101 no tiene result.json y no hay QEMU ni
runner activos. El ultimo serial llega a1359.89s, despues de cargar wevtsvc;
ABI y07/23/25/30 tienen status0, MountMgr arranca. La causa de la interrupcion
no queda registrada: esta prueba NO demuestra salida0 ni persistencia. W3-99
es el ultimo resultado cerrado de Wine y falla con c000011f (limite de FD)
antes de probar el kernel512FD. Siguiente paso: repetir el gate con un plazo
externo suficiente y conservar result.json; si falla, analizar el primer error
que lleva a salida no cero. Solo tras salida0 consultar registro, ejecutar
verify-prefix.py sobre copia del disco y empaquetar los binarios validados.

Medición de almacenamiento 2026-09-27: AHCI puede arrancar la imagen raíz
FAT. W3-AHCI-01 alcanzó "Operation done" en el proceso instalador, pero el
proceso inicial falló después al cargar kernelbase.dll (launcher256); no es
un gate aprobado. La inspección del prefijo mostró varias DLL grandes
ausentes. Se descubrió que read/write de archivos rechazaban llamadas
mayores de64KiB; ahora se ejecutan en fragmentos internos, y caso25 verifica
una llamada de128KiB en cada dirección. La siguiente prueba Wine se detuvo a
pedido del usuario antes de QEMU, por lo que este arreglo aún no demuestra
wineboot completo. El informe [STORAGE_MEASUREMENTS_2026-09-27.md](STORAGE_MEASUREMENTS_2026-09-27.md)
contiene mediciones pareadas IDE/AHCI, contadores de bloques y referencia
TCG/KVM. Se prioriza medir y optimizar antes de otro wineboot largo.

## W3: inicialización y persistencia aprobadas (2026-09-27)

El análisis del código de Wine 11.0 encontró que `ntdll/unix/env.c` llama a
`wineboot.exe --init` automáticamente antes del primer proceso PE de un
prefijo nuevo. El launcher también lo lanzaba de forma explícita, y cada
`wine reg.exe` sucesivo volvía a arrancar Wine. Una corrida de 1536 MiB
anterior a la corrección tardó 632,21 s y registró tres `Operation done`.
El launcher actual usa un solo ejecutable PE de prueba para consultar
MountMgr/ImagePath y ProductName, escribir/leer `W3-PERSIST-v1` y luego espera
que wineserver guarde los hives. El primer arranque ejecuta la inicialización
upstream automáticamente una sola vez.

También se redujeron recorridos FAT redundantes: el buscador de huecos de
directorio detiene el recorrido al encontrar el marcador final, el puenteo de
marcadores antes de una entrada LFN sólo se ejecuta cuando hace falta, el
caché de sectores conserva los bytes después de una escritura correcta y la
asignación de clusters reutiliza un punto de búsqueda por volumen con
validación de la FAT. El presupuesto de VM crece con la RAM; la tabla de
pares IPC sube a 512, los recursos syscall a 64 y los procesos de paginación
a 64. Esto eliminó los `pair table full` y `fail=vm-resources` observados en
pruebas anteriores. El gate ahora considera estos avisos errores fatales.
No hay una corrida A/B que permita atribuir una cantidad concreta de segundos
a cada cambio FAT o de memoria.

`build/w3-auto-init-kvm-1536-01`: KVM, AHCI, una CPU, 1536 MiB RAM, imagen
FAT32 de 768 MiB. ABI y casos 23/25/28/30 pasaron, el proceso Wine salió 0,
las consultas de registro y los tres hives persistidos pasaron. Tiempo de
pared del gate: **277,52 s**; `Operation done` a 270,10 s del guest. Sin
faults fatales ni agotamiento de IPC o VM.

`build/w3-auto-init-kvm-1536-reboot-01`: copia del mismo disco y kernel;
ABI y los mismos casos pasan. Se leyó el marcador persistido sin recrearlo,
con salida 0 de Wine y wineserver. Tiempo de pared: **482,46 s**; la secuencia
de servicios de Wine volvió a registrar `Operation done` a 475,16 s. Esta
segunda duración no mide la instalación del prefijo desde cero. Persisten
avisos de `winebth` (servicio Bluetooth) y `explorer` (no hay controlador de
ventanas/X11), pero no impiden estas validaciones. El aviso de FSInfo
desactualizado es real: en la imagen del primer gate el campo declara 98.946
clusters libres y la FAT contiene 88.900; tras el reinicio, 88.813. FSInfo
es orientativo y el kernel calcula el espacio libre desde la FAT confirmada,
por lo que no intervino en asignaciones ni en los tiempos de Wine. Mantenerlo
sincronizado requiere escribir los sectores FSInfo principal y de respaldo
en un punto de sincronización; hacerlo en cada cambio de cluster añadiría
dos escrituras durables a miles de archivos pequeños, así que queda como
limitación de metadatos en disco.

`build/w3-fat-all30-kvm-1536-01`: ABI y los 30 casos POSIX pasan con el mismo
kernel, KVM y AHCI, en 115,87 s de pared, sin faults fatales. Las pruebas de
layout HighMem y reservas de stage2/kernel también pasan.

No se copió código fuente de otros sistemas operativos en estos cambios.

## Repetición en la misma imagen, sin copia (2026-09-27)

Se repitió la prueba solicitada desde un prefijo ausente en
`build/w3-cold-sameimg-init-01/test.img`, con KVM, AHCI, una CPU, 1536 MiB de
RAM y FAT32 de 768 MiB. ABI, casos 23/25/28/30 y launcher salieron 0. El
primer `wineboot --init` automático registró `Operation done` a 233,27 s de
tiempo invitado; el gate completo tomó **240,35 s** de pared y guardó los tres
hives y el marcador `W3-PERSIST-v1`.

El segundo comando usó `verify-prefix.py --normal-start --in-place`; QEMU
abrió exactamente el archivo anterior, con el mismo inode 4725639 y sin
`test.img` en el directorio de la segunda corrida. El launcher de prueba se
reemplazó dentro de esa imagen para comprobar el prefijo persistido, sin
formatear ni volver a instalarlo. ABI, los mismos cuatro casos y Wine
salieron 0, se leyó el marcador original y el gate antiguo no detectó fallos
fatales. El
segundo gate tomó **372,12 s** de pared (**6 min 12 s**). Resultados y log:
`build/w3-cold-sameimg-reboot-01/`. El segundo tiempo mide el primer inicio
de Wine después de reiniciar QEMU, no sólo la carga del kernel. Esta corrida
se reclasificó como no aprobada al detectar un timeout de Wine que el gate
anterior había ignorado; ver la revisión siguiente.

## W3: perfil acumulado y revisión del gate (2026-09-27)

Corrección de la interpretación anterior: el log del reboot de 372,12 s
contenía el error de Wine «boot event wait timed out». El gate anterior
no lo trataba como fatal: su PASS no demuestra un inicio normal. Ese
timeout interno de 300 s explica plausiblemente la demora, pero el fixture
original fue eliminado externamente y no permite atribuirla con certeza.
Los dos verificadores ahora rechazan ese marcador.

Se añadió --profile-w3 sólo para el kernel de prueba. Acumula llamadas,
tiempo inclusivo, máximo y cinco rangos de tamaño de I/O en memoria, e
imprime una tabla al final. Incluye syscalls i386, VFS, FAT, block, AHCI
e IPC. El launcher imprime hitos de inicio, fin de Wine, cierre de
wineserver y comprobación de hives. Los timestamps de Wine ubican el
arranque de wineserver, wineboot, servicios, MountMgr, explorer y SetupAPI.
Los tiempos inclusivos de hilos que esperan se superponen: no son
porcentajes de pared ni se deben sumar entre capas. El perfil exige una CPU.

Al desaparecer el antiguo current-rootfs, se reconstruyó el fixture desde
build/source/bleskernos-source.img, build/mbr.bin y build-runtime.py. Los datos
quedaron en /home/lucas/w3-profile-*; todas las filas usan el mismo origen,
KVM, AHCI, una CPU, 1536 MiB RAM, FAT32 de 768 MiB y Wine 11.0 completo.
Sus tiempos absolutos no se comparan con los 240/372 s del fixture anterior.

| corrida | gate pared | Wine desde launcher | flush AHCI: operaciones / ms | resultado |
| --- | ---: | ---: | ---: | --- |
| baseline-init-03 | 145,7 s | 126,5 s | 10.145 / 10.155 | PASS |
| baseline-reboot-03, mismo test.img | 136,2 s | 116,3 s | 9.599 / 9.971 | PASS |
| baseline-init-04 | 190,9 s | 168,2 s | 10.225 / 46.342 | PASS |
| baseline-init-05 | 154,9 s | 135,8 s | 10.140 / 14.697 | PASS |
| baseline-reboot-05, mismo test.img | 147,1 s | 125,3 s | 9.598 / 10.559 | PASS |
| baseline-init-06 | 161,8 s | 138,2 s | 10.138 / 10.675 | PASS |

La ejecución fría varía 45 s con casi el mismo número de operaciones.
Entre init-03 e init-04, el flush AHCI añade 36 s y Wine crece 42 s.
Los 4,9 millones de resoluciones FAT acumulan 7,3–8,5 s y 1,15 millones
de búsquedas de directorio 5–6 s. FAT/VFS no explica esa dispersión.
En init-05, mmap2 suma ~42 s inclusivos/73.823 llamadas, openat
19.750/13,2 s y statx 46.195/10,9 s. read y poll incluyen esperas
solapadas de Wine: 1.029 s y 99 s acumulados no equivalen a CPU activo.
IPC registra 210.523 yields y 85.824 sleeps de un tick en frío; en el
reboot de la misma imagen, 212.121 yields y 142.664 sleeps.

El segundo arranque preservó el prefijo y W3-PERSIST-v1; sí volvió a
ejecutar el wineboot.exe --init automático previo al primer proceso PE,
con servicios, explorer y SetupAPI. En este fixture no reapareció el
timeout: ambos reboots fueron más rápidos que sus primeros arranques.
En frío, la fase más larga observada va del segundo rundll32 (~43 s
del reloj Wine) a Operation done (~145 s): incluye instalación/copia
SetupAPI y servicios. El reboot hace de nuevo esa secuencia sobre el
registro existente, sin crear el prefijo.

Experimentos aislados; todos pasaron ABI y casos 23/25/28/30:

| variante | gate frío | flush AHCI | decisión |
| --- | ---: | ---: | --- |
| cache=none | 195,2 s | 45,2 s | descartada; igual que una baseline lenta |
| 64 yields antes de dormir | 194,8 s | 44,8 s | revertida; sleeps suben a 92.022 |
| PIT 300 Hz en P6 | 193,9 s | 48,6 s | revertida; sleeps suben a 139.447 |
| cache=writethrough | 162,4 s | 2,5 s | no adoptada; trasladó costo a escritura y empató con baseline-init-06 (161,8 s) |
| QEMU -cpu host | 203,1 s | 45,7 s | no adoptada; coincidió con flushes lentos |

Ninguna variante produjo una ganancia reproducible; el kernel productivo
conserva 100 Hz en Pentium III, 16 yields y la escritura AHCI durable.
Tampoco se usa cache=unsafe ni se omiten flushes: el gate exige
persistencia. FSInfo queda como problema separado. El cambio de
comportamiento permanente del gate es rechazar el timeout de Wine que
antes producía un falso PASS.

Validación final con el profiler y el gate corregido: 
final-init-all30 pasó ABI, 30/30 casos POSIX, primer wineboot, consultas
de registro y hives persistidos en 235,2 s de pared. Wine desde el launcher:
178,3 s; 10.185 flushes AHCI sumaron 46,6 s. Luego
final-reboot-all30 arrancó exactamente el mismo test.img mediante
verify-prefix.py --in-place --normal-start: volvió a pasar ABI, 30/30,
lectura del marcador, inicio normal de Wine y cierre de wineserver en
237,8 s. Wine desde el launcher: 178,9 s; 9.878 flushes sumaron 37,7 s.
No apareció el timeout interno. El reboot registró 193.110 sleeps IPC
frente a 95.236 en frío y 191.845 mmap2 frente a 73.824; sus tiempos
inclusivos se solapan. Ambos resultados y logs están en
/home/lucas/w3-profile-final-{init,reboot}-all30.

Compilar el kernel productivo, comprobar el layout HighMem, la reserva
stage2/kernel y la sintaxis de los scripts Python también pasó. No se
copió código de otros sistemas operativos en esta instrumentación.
