# BlesKernOS 1.0: comparación arquitectónica y puntos débiles

Fecha: 2026-10-13. Esta revisión precede a nuevas correcciones de producción.
Compara contratos de seguridad, ownership, concurrencia y recuperación, no
cantidad de funciones ni popularidad. No se ejecutaron benchmarks comparativos
con otros OS. No se modificaron kernel, drivers, GUI, aplicaciones o imágenes.

## Diagnóstico principal

BlesKernOS ya tiene mecanismos reales de un kernel con procesos: Ring 3,
tablas privadas, paging con CR0.WP, guards, pager, swap, mappings de archivo,
COW de vistas privadas, scheduler SMP, FPU por tarea, journal y diagnóstico.
Las etapas anteriores corrigieron rangos del pager, protegieron la tabla de
mappings y repararon comparación TLB al desbordarse la generación.

La debilidad dominante es **una frontera de confianza todavía incompleta**.
Memoria de aplicaciones y varias estructuras de control comparten heap;
la ABI nativa permite ejecutar muchas funciones internas en Ring 0 con
argumentos que solo se validan como palabras en una pila. Algunas protecciones
de paging pueden quedar eludidas por ese camino.

El cambio más útil es consolidar esos contratos, antes de aumentar paralelismo
de Wine, soporte de hardware o complejidad del scheduler. Una nueva página
supervisor no basta si una API puede escribir en ella por cuenta del usuario.

## Evidencia y grado de certeza

- **C:** mecanismo o debilidad confirmado por código inspeccionado.
- **R-host:** comportamiento reproducido con funciones de producción y mocks.
- **R-guest previo:** reproducción/prueba QEMU guardada en etapas anteriores.
- **H:** escenario de fallo razonable, aún sin reproducción de alcanzabilidad.

No se usan porcentajes de seguridad ni etiquetas de subsistema «completo».
El fingerprint incluye 228 archivos fuente/headers/ASM/linker de `kernel/`;
no significa que se hayan auditado exhaustivamente sus 228 contenidos.
El inventario del loader contiene 677 nombres únicos declarados con `EXPORT`;
es un conteo estático, no una medición de permisos efectivos por proceso.

Evidencia nueva en `tests/kernel/evidence/comparison-2026-10-13/`:
`kernel-before.json`, `public-export-inventory.json`, `references.json`,
fuentes upstream fijados por commit, `native-api-reproduction.log` y
`elf-alignment-reproduction.log`.
Se reutilizan referencias y resultados de las etapas 1/2, sin afirmar que
aquellas 24 pruebas se hayan vuelto a ejecutar durante esta revisión.

## 1. Ring 0/Ring 3 y API nativa — prioridad P0

**BlesKernOS:** `elf_symbol_value()` resuelve imports de usuario mediante
`elf_user_api_thunk()`. `SYS_API_CALL` valida token, pila y copia palabras;
`elf_user_api_dispatch()` llama `elf_api_call_raw()` con esos argumentos.
El catálogo común incluye `memcpy`, `memset`, `kmemcpy`, `kmemset`, `kmalloc`,
`kfree`, `task_get`, operaciones VFS y numerosas funciones GUI. El catálogo
privado de Vision existe, pero no elimina los exports comunes anteriores.

`kernel/libc/string.c` dirige `memcpy` a `kmemcpy`. El guard de la cabecera raíz
del heap en `kmemcpy/kmemset` conserva algunos campos de esa cabecera; no valida
arbitrariamente origen, destino, longitud y propietario del objeto apuntado.

Otro camino de mayor alcance es `elf_preview_create`, incluido en el catálogo
común: carga la imagen con `user_image=false` y llama directamente a
`entry(desktop)` sin crear una tarea de usuario. Si se invoca a través de la
API nativa, ese registro/preview ejecuta código de la imagen en el contexto
Ring 0 del dispatcher. No se observa una comprobación de rol del llamante en
esa función. Es una ruta C comprobada; no se ejecutó un preview adversarial.
Por tanto, proteger solo los helpers de memoria dejaría otra frontera abierta.

**R-host:** `audit_native_api_boundary.py` ejecutó el dispatcher y `kmemcpy`
reales, con catálogo/llamada ASM/contexto simulados. Un token válido escribió
en un sentinel que representa storage del kernel. El token fuera de rango sí
fue rechazado. Esto verifica la ausencia de una barrera de punteros en ese
camino; no es una explotación completa Ring 3→Ring 0 dentro de QEMU.
El diagnóstico termina 0 al reproducir el problema: **no es una regresión de
seguridad verde** y no forma parte del conteo de 24 tests anteriores.

**Referencia:** Linux y BSD proporcionan operaciones de copia usuario/kernel
que pueden devolver errores de acceso. SerenityOS exige un rango de usuario
al construir el buffer de syscall y conserva esa distinción durante la E/S.
[Linux uaccess](https://docs.kernel.org/core-api/mm-api.html),
[NetBSD copy(9)](https://man.netbsd.org/copy.9),
[OpenBSD copyin(9)](https://man.openbsd.org/copyin.9),
[FreeBSD copy(9)](https://github.com/freebsd/freebsd-src/blob/main/share/man/man9/copy.9),
[Serenity buffer de usuario](https://github.com/SerenityOS/serenity/blob/7c0540a22900a8366741205dead7f76ae4cfabcc/Kernel/Library/UserOrKernelBuffer.h), [Serenity read](https://github.com/SerenityOS/serenity/blob/9ad0303622f37088feaf54a6ef8b689deb1a13ff/Kernel/Syscalls/read.cpp).

**Reforzar conservando identidad:** mantener tokens y ABI, clasificar exports,
crear wrappers con tipos/dirección/tamaño, rechazar buffers supervisor, controlar
ownership y convertir objetos privilegiados en handles validados. Llevar
helpers puros y libc a las aplicaciones cuando su ABI lo permita.
La primera barrera de páginas no resuelve accesos entre procesos que comparten
el heap; eso exige la separación posterior de allocations y metadata.

**Prueba antes de corregir:** fixture guest con canary supervisor descartable,
imports nativos reales y buffers nulos/kernel/readonly/overflow. Debe rechazar
la operación sin cambiar el canary ni detener Vision, y conservar casos válidos
de memcpy, strings, stdio y GUI. No usar estructuras reales del kernel como
objetivo de la reproducción.

## 2. Memoria, LDT y estructuras de control — prioridad P0

**C:** `paging_configure_user_layout()` publica heap y ventana PE histórica
a usuario. El guard de la primera cabecera no convierte todo el heap en
supervisor. La tabla VM ahora usa frames supervisor; otras clases no:

| Clase | Allocation observada | Consecuencia que debe probarse |
|---|---|---|
| LDT Linux | `sys_linux_modify_ldt()`, kmalloc de 65536 bytes; la CPU la consume mediante `gdt_activate_user_ldt()` | Modificación directa posterior puede saltarse la validación del descriptor |
| Tablas FD/open flags | `resource_for()`, kmalloc de `syscall_fd_table_t` | Alteración de descriptores/derechos de otro proceso o backend |
| Metadata del heap | `heap_block_t` dentro del heap, enlaces y owner | Escrituras directas pueden corromper lista y ownership |
| Pager y cache de archivo | `region_allocate()` intenta kzalloc; chunks/source usan heap | Alteración de pins, frames y referencias; no todos los objetos VM están protegidos |
| BFS4 | `bfs4_mount()` reserva bitmaps, inodes e índices con kmalloc/kzalloc | Corrupción de datos de control puede convertirse en escrituras incorrectas en disco |
| Módulos residentes/drivers | `driver_load()` → `elf_load_resident()` → `elf_load(...,false)`, imagen reservada con kzalloc | Verificar protección supervisor de código/datos que luego ejecutan callbacks Ring 0 |
| FPU/contexto auxiliar | `task_fpu_alloc_buffer()` usa allocation dinámica | Corrupción del estado restaurado necesita prueba adversarial |

La presencia en heap publicado es evidencia de exposición; corrupción concreta
y escalada de privilegios no se declaran reproducidas en guest en esta revisión.
Existen rangos del heap protegidos explícitamente, por ejemplo stacks kernel;
la revisión de placement no sustituye una inspección de PTE de cada allocation.

**Referencia:** Linux reserva LDT en memoria kernel y sincroniza su instalación
en CPUs que usan el mm. SerenityOS separa regiones kernel y protege texto;
FreeBSD reserva objetos de VM mediante zonas propias. [Linux LDT](https://github.com/torvalds/linux/blob/3eaa50e1e255ec261c757b9eea811ef3bac10d1e/arch/x86/kernel/ldt.c),
[Serenity MemoryManager](https://github.com/SerenityOS/serenity/blob/fb23f3c90282f916a74a062ba6a6d4e8c9e884c0/Kernel/Memory/MemoryManager.cpp),
[FreeBSD vm_map](https://github.com/freebsd/freebsd-src/blob/main/sys/vm/vm_map.c).

**Reforzar:** allocator para objetos supervisor, con inventario de callers y
fallo limpio; empezar por LDT, FD y metadata de ownership, después pager/BFS4.
No mover indiscriminadamente surfaces y buffers que clientes usan directamente.
Para LDT hay que probar activación, readback, clone, cambio de CPU y retirada
antes de reutilizar backing; cambiar kmalloc por frames solo cubre una parte.

**Pruebas:** lectura/escritura directa desde Ring 3 rechazada; `modify_ldt`
válido y malformed; fork/borrow/exit; frames recuperados; descriptor en uso
por otra CPU no liberado; OOM sin fallback a memoria de usuario. Para BFS4,
intento de alteración de metadata y fsck posterior en imagen temporal.

## 3. Lifetime VM, copias y TLB — prioridad P0/P1

**C/H:** `paging_process_borrow()` conserva IDs de préstamo, sin contador de
referencias en esa estructura. `task_process_can_release()` sí revisa tareas
del mismo process_id: no corresponde afirmar que no existe protección del
reaper. Falta demostrar su interacción con borrowers de otro ID, cambio de CR3
y copias activas.

`syscall_process_cleanup()` descarta resultados de unmap y sigue liberando
backing. `pager_destroy()` no comprueba pins antes de liberar; `paging_process_destroy()`
retira el registro y libera tablas. La espera TLB puede terminar por timeout.
El resultado es un riesgo de reutilización con referencias/traducciones activas,
cuya alcanzabilidad SMP necesita una fixture específica.

`copy_from/to_user()` fija páginas del pager, comprueba permisos y hace memcpy.
Mappings legacy pueden no estar en el pager. Validar antes de copiar no garantiza
que un cambio concurrente de permisos/destrucción no produzca un fault en Ring 0.
Los helpers revisados no contienen un fixup específico para esa copia.

**Referencia:** Linux distingue invalidación y liberación; NetBSD separa
remoción de mappings y desapego de objetos; Haiku retiene referencia al address
space durante wiring/unwiring. [Linux mmu_gather](https://github.com/torvalds/linux/blob/1fb3d8c20bfadbbe2d9e5de18074de9282a52b5f/mm/mmu_gather.c),
[NetBSD uvm_map](https://github.com/NetBSD/src/blob/1fc61a5c45cdd2a67b7e1b272c41a67aa200f621/sys/uvm/uvm_map.c),
[NetBSD UVM](https://man.netbsd.org/uvm.9),
[Haiku VM](https://github.com/haiku/haiku/blob/master/src/system/kernel/vm/vm.cpp).

**Reforzar:** estado vivo/retirándose/retirado, referencias del espacio,
borrowers y pins; revocación confirmada antes de free; cuarentena cuando falla
ACK; copias recuperables con salida EFAULT. No convertir timeout en éxito ni
solucionarlo aumentando arbitrariamente el número de spins.

**Prueba:** CPU A copia, CPU B protect/unmap/exit, con CLONE_VM/vfork; ACK
retrasado o ausente y generación próxima a wrap. Ningún frame/tablas debe
reutilizarse hasta terminar referencias e invalidaciones. La cuarentena debe
ser observable y acotada para no esconder una fuga indefinida.

## 4. Scheduler, IRQ, locks y SMP — prioridad P1

**C:** colas por CPU, claims atómicos, work stealing y afinidad; TSS/TLS y
save/restore FPU. La prioridad ajusta quantum. Kernel/legacy y tareas Linux
tienen restricciones de ubicación; Linux se limita a CPU1 cuando hay SMP.
Esto es una decisión explícita de estabilidad, no prueba de concurrencia libre.

`kernel_domains` ordena nueve dominios y registra inversiones, pero continúa
la adquisición. Las clasificaciones dependen en parte de nombres/prefijos de
API; no constituyen contratos de permisos ni un grafo de todos los locks.
Spinlocks/mutex/rwlocks recursivos saturan profundidad a 0xffff sin registrar
una adquisición adicional. Hay oportunidad de diagnósticos de overflow,
ownership incorrecto y sleep bajo contexto no permitido.

**Referencia:** Linux lockdep sigue dependencias y estados IRQ; FreeBSD WITNESS
detecta órdenes de locks. Linux EEVDF es una política de scheduling distinta;
no se propone importarla ni se asume que resulte mejor para esta carga.
[lockdep](https://docs.kernel.org/locking/lockdep-design.html),
[WITNESS](https://man.freebsd.org/cgi/man.cgi?query=witness&sektion=4),
[FreeBSD subr_witness](https://github.com/freebsd/freebsd-src/blob/d9e2caccfac14c43bf9ad62bdd35d4a03356ff91/sys/kern/subr_witness.c),
[EEVDF](https://docs.kernel.org/scheduler/sched-eevdf.html).

**Reforzar:** contratos explícitos de cada API/contexto, registro de locks
individuales en debug, invariantes de tareas/runqueues y asserts de ownership.
Medir latencia p95/p99 de wakeup/input con carga, no solo FPS o arranque.
Mantener afinidad Linux hasta pasar lifecycle/VM/signals bajo carga sostenida.

**Pruebas:** tarea en una sola cola/CPU; sleep/wakeup/kill simultáneos; exit
reteniendo locks; IRQ durante handoff; FPU/SSE sin mezcla entre tareas; violación
deliberada de orden en build debug. El log de una inversión no es recuperación.

## 5. VFS, BFS4 y persistencia — prioridad P1

**C/R-guest previo:** BFS4 tiene undo journal de metadata, CRC/checkpoints,
serialización y `write_failed`. Las pruebas anteriores cubren cortes y fsck
host. ATA ejecuta CACHE FLUSH; AHCI FLUSH CACHE EXT; USB SYNCHRONIZE CACHE(10).
La escritura volátil de swap tiene un camino separado. No falta flush en estos
backends inspeccionados.

El journal es opcional para imágenes antiguas y no cubre datos de archivo.
La durabilidad depende de las garantías reales del dispositivo, del orden,
de close/sync/writeback y de su propagación al caller. Un journal de metadata
es una elección válida; no equivale a prometer atomicidad de todo contenido.
La nueva exposición de bitmaps/inodes al heap de usuario es prioridad de
integridad antes de optimizar caches o cambiar formato.

**Referencia:** la API JBD2 Linux explicita transacciones, handles y aborts.
Es un contrato útil para revisión; no obliga a sustituir BFS4 por ext4.
[Journalling API](https://docs.kernel.org/filesystems/journalling.html).

**Pruebas:** fallos read/write/flush independientes por backend, torn writes,
hot-unplug, remontaje, coherencia MAP_SHARED/file cache, fallo de close/fsync,
rollback de operaciones y datos no relacionados intactos. No dar por probado
hardware USB/AHCI por pasar un modelo host ATA.

## 6. Permisos y autorización — prioridad P1

**C:** la ABI Linux expone uid/gid sintéticos 0. `sys_linux_faccessat()` aplica
semántica de root sintético y valida flags/X_OK; no es un modelo de cuentas
aisladas. En el catálogo nativo existen operaciones globales de VFS, mount,
procesos y configuración. Validar que un token existe no decide quién puede
usarlo ni qué objetos puede afectar.

**Referencia:** OpenBSD pledge restringe operaciones disponibles al proceso;
unveil limita rutas visibles. Son mecanismos adicionales a validación de
punteros y protección de memoria. [OpenBSD kern_pledge](https://github.com/openbsd/src/blob/1c37ded5694cf0fe50d6c9e17c7176daf7afedc7/sys/kern/kern_pledge.c),
[pledge](https://man.openbsd.org/pledge.2),
[unveil](https://man.openbsd.org/unveil.2).

**Reforzar:** distinguir aplicación, servicio y administrador; derechos por
operación/handle y ownership de recursos. Mantener la identidad de BlesKernOS:
no hace falta implementar de inmediato todas las cuentas Unix ni copiar pledge.
Las capabilities propuestas no compensan un heap de control modificable.

**Pruebas:** aplicación normal no monta, cambia drivers, mata servicios o edita
archivos de sistema sin autorización; una herramienta autorizada conserva esas
operaciones. Reconocer y probar las excepciones necesarias para Vision y Wine.

## 7. Vision, GUI y servicios — prioridad P1/P2

**C/R-guest previo:** Vision ya ejecuta motor/compositor en Ring 3; tiene
exports privados, IPC, upcalls y limpieza por proceso. Varias llamadas GUI y
render históricas regresan a Ring 0. No se justifica tratar toda la GUI como
si aún ejecutara permanentemente en modo kernel.

**Referencia:** Haiku `ServerApp` vincula puertos al team del cliente y maneja
objetos/ventanas del servidor; ilustra protocolo y ownership. [Haiku ServerApp](https://github.com/haiku/haiku/blob/9661ecabc2b8b461ddb0bf38b0cfd07c7e08c4e7/src/servers/app/ServerApp.cpp).

**Reforzar:** política de ventanas y procesamiento gráfico en Vision/userland,
buffers compartidos explícitos, mensajes acotados y handles con generación.
Conservar en kernel drivers y acceso privilegiado. Migración por familias de
API, manteniendo callbacks y layouts que las aplicaciones realmente utilizan.

**Pruebas:** muerte/reinicio de Vision, mensajes malformados, cliente muerto
durante frame/callback, handles stale, cola llena, timeouts y aplicación legacy
usando la misma operación antes/después. No borrar interfaces para mejorar una
métrica de «código en Ring 0» sin probar compatibilidad.

## 8. OOM, protección de imagen y diagnóstico — prioridad P1/P2

**C/R-guest previo:** allocator de runs contiguos y heap general; administración
de una región utilizable contigua de E820. Demand paging/swap no eliminan
fragmentación de allocations kernel ni agregan automáticamente toda RAM discontinua.
El perfil 16 MiB falló por OOM/fragmentación con kernel anterior y modificado.

CR0.WP está activo, pero el código de `paging_init()` mantiene writable las
PTE del kernel en el identity map bajo 4 MiB; flags R-X del linker no programan
por sí mismos las PTE. No hay NX independiente con las tablas actuales.
Protección de texto/rodata merece revisión de escrituras legítimas de arranque
antes de cambiar permisos; NX/PAE es una etapa aparte según hardware soportado.

Hay logs, captura serial, pantalla panic y watchdog. `panic_show()` utiliza
consola, dominios, gráficos y sonido. El watchdog se evalúa desde CPU0: una
CPU0 bloqueada no garantiza que siga diagnosticando a las demás. Son riesgos
H de recuperación incompleta, no un panic-deadlock reproducido aquí.

**Referencia:** SerenityOS programa permisos de texto/datos explícitamente;
Linux diferencia allocation que puede dormir/reclamar de la que no debe iniciar
E/S, y contempla reservas para avance bajo presión.
[MemoryManager](https://github.com/SerenityOS/serenity/blob/fb23f3c90282f916a74a062ba6a6d4e8c9e884c0/Kernel/Memory/MemoryManager.cpp),
[Allocation controls](https://docs.kernel.org/core-api/mm-api.html).

**Reforzar:** reservas pequeñas para reclaim/diagnóstico, caches descartables,
fallo acotado de UI y recuperación de recursos; texto/rodata readonly donde sea
compatible; salida de emergencia sin locks y registro CR2/CR3/EIP/PID/CPU.
Probar panic con locks retenidos y heap agotado; no depender de GUI disponible.

## 9. Cargadores ELF y módulos — prioridad P1

**C/R-host:** `elf_align()` suma antes de redondear sin comprobar overflow.
El cálculo `image_size + image_alignment - 1 + sizeof(void *)` usado en las
dos rutas de carga de objetos ELF tampoco comprueba su suma completa.
Las validaciones de tamaño de sección y alineación potencia de dos no cubren
ambas operaciones.

`audit_elf_alignment.py` ejecuta el helper y la expresión reales en i386,
sin asignar/cargar ninguna imagen. Con un tamaño acumulado de 2147483647 y
alineación 2147483648, potencia de dos válida para el check actual, la reserva
calculada es de **2 bytes**. El redondeo de 0xfffffff8 a 16 también vuelve a cero.
La aritmética incorrecta está reproducida; no se ha demostrado todavía un ELF
malformado completo que alcance corrupción en guest.

**Reforzar:** suma/multiplicación/redondeo comprobados antes de allocation,
límites del formato y del kernel, y validación de secciones/relocations contra
el tamaño real reservado. Separar permisos de imagen de proceso y módulo
privilegiado; no deducirlos solo de `user_image` o del tipo de fichero.
Los límites y redondeos explícitos revisados en VM de Serenity/Haiku son
patrones útiles; no prueban que sus loaders carezcan de bugs.

**Pruebas:** ELF truncado, alineación enorme/no potencia de dos, overflow de
tamaño total, NOBITS grande, relocations y string/symbol tables fuera de rango;
OOM y cleanup sin leaks. Usar fixtures host y guest desechables, preservando
los módulos DVR y BEX válidos. Los diagnósticos de esta revisión no ejecutan
código de un fichero malicioso.

## Matriz de decisión

| Prioridad | Debilidad | Certeza | Impacto | Riesgo del cambio |
|---|---|---|---|---|
| P0 | API nativa permite punteros sin contrato y preview directo privilegiado | C + R-host parcial | Lectura/escritura o código fuera de frontera de confianza | Alto para ABI; dividir en wrappers y roles |
| P0 | LDT/FD/heap/pager/BFS4 control en heap de usuario | C; ataque guest pendiente | Corrupción de procesos, privilegios y disco | Medio/alto por ownership y memoria |
| P0/P1 | Teardown/borrowers/pins/ACK fallido sin retirada demostrada | C + H | Use-after-free y reutilización bajo SMP | Alto; refs/cuarentena antes de free |
| P1 | Locks gruesos, inversión registrada pero permitida, contexto IRQ | C + H | Deadlocks/latencia | Medio; debug/invariantes primero |
| P1 | Root sintético y autorización limitada | C | Acceso global no segmentado | Alto para política de apps/servicios |
| P1 | Persistencia/coherencia no certificada por backend | Parte probada, parte H | Datos y recuperación | Alto si toca disco; fixtures aisladas |
| P1 | Overflow de alignment/tamaño total en loader ELF | C + R-host aritmético | Allocation insuficiente; corrupción guest pendiente | Bajo/medio; rechazos acotados |
| P1/P2 | OOM de Vision/heap fragmentado y reservas | R-guest previo | Desktop no disponible con poca RAM | Medio; medir y reducir allocation |
| P2 | Texto readonly/NX y diagnóstico de emergencia | C/H | Daño por escrituras/fallo sin log | Medio/alto según compatibilidad |
| P2 posterior | Afinidad Wine y fork anónimo con copia de páginas residentes | C | Escalabilidad/costo de fork | Alto; después de lifetime/aislamiento |

## Orden recomendado para el refuerzo siguiente

1. Fijar modelo de confianza y catálogo de exports; reproducir frontera nativa
   en guest. Agregar wrappers a helpers de memoria y restringir carga/preview
   privilegiados antes de certificar nuevas protecciones supervisor.
2. Proteger LDT/FD/metadata del allocator y extender el allocator supervisor
   a objetos pager/BFS4, por grupos pequeños con OOM/cleanup/compatibilidad.
   El overflow ELF admite una corrección independiente y acotada sin esperar
   a completar esta migración; preparar primero fixtures malformed/OOM.
3. Lifetime VM y retirada confirmada, incluyendo borrowers y copias recuperables;
   no liberar backing ante fallos de ACK.
4. Migrar allocations/ELF/stacks/TLS nativos a mappings privados y eliminar
   gradualmente PAGE_USER del heap de control. Mantener buffers compartidos
   autorizados en un contrato separado.
5. Con invariantes y stress de IRQ/locks, revisar scheduler y paralelismo Linux.
   Después reforzar E/S por backend, recuperación y perfiles de RAM baja.

Estos pasos no son una reescritura hacia otro OS. Retienen formatos BFS4,
tokens/ABI cuando sea posible, Vision y soporte legacy; cada cambio debe tener
reproducción previa, regresión y rollback. El objetivo de esta revisión es
seleccionar correcciones por evidencia, sin implementar todavía la siguiente etapa.
