# Refuerzo de memoria y API nativa — 2026-10-13

Se refuerzan los helpers de memoria de Ring 3, la metadata del pager y la
liberación de espacios prestados. La ABI y las aplicaciones existentes se
mantienen. El heap plano heredado todavía necesita una migración para conseguir
el aislamiento completo entre kernel y aplicaciones.

## Ideas tomadas de Linux

Se estudiaron las implementaciones y documentación, sin incorporar código:

- [`include/linux/uaccess.h`](https://github.com/torvalds/linux/blob/master/include/linux/uaccess.h):
  comprobar rangos y tamaños antes de copiar, distinguir copias de usuario de
  primitivas internas y comprobar sus resultados.
- [`kernel/fork.c`](https://github.com/torvalds/linux/blob/master/kernel/fork.c),
  `mmput`, `__mmput` y `__mmdrop`: conservar referencias al espacio de memoria y
  verificar que ninguna CPU lo use antes de liberar sus tablas.
- [Documentación de refcount y orden de memoria](https://docs.kernel.org/core-api/refcount-vs-atomic.html):
  sincronizar adquisición de referencias y destrucción. En BlesKernOS estas
  operaciones usan su spinlock de paging y rechazan saturación; no se importó
  `refcount_t` ni la estructura `mm_struct` de Linux.

## Frontera de la API

`kernel/api/native_memory.c` intercepta por dirección real de la función,
independientemente del nombre del import:

- `kmemcpy`, `memcpy`, `memmove`, `kmemset`, `memset`, `kmemcmp`, `memcmp` y
  `bk_memory_compare`.
- `kstrlen`/`strlen`, `kstrcmp`/`strcmp`, `kstrncmp`/`strncmp`,
  `kstrcpy`/`strcpy`, `kstrncpy`/`strncpy`, `kstrcat`/`strcat` y sus equivalentes
  `bk_string_*` expuestos por el SDK.
- `kfree`/`free`/`bk_sys_free`, `krealloc`/`realloc`/`bk_sys_realloc`,
  `mm_get_system_info`/`bk_sys_memory_info` y los mutadores de ownership/subsistema.

Las operaciones comprueban overflow, permisos efectivos y residencia/pinning.
Para alias del heap se comprueba que el rango esté dentro de payload ocupado:
no se aceptan headers, bloques libres ni allocations privadas de otro proceso.
Un buffer conserva un pin de heap además de los pins del pager. Los mappings
privados no se confunden con el heap que ocupaba anteriormente su dirección
virtual. Un fallo libera los pins adquiridos y devuelve un resultado nulo con
`EFAULT`/`EACCES` cuando corresponde.

Las strings se recorren por fragmentos de página y de allocation, con un límite
de búsqueda de 1 MiB. La escritura utiliza la longitud verificada, y escribe el
terminador explícitamente: una modificación concurrente del NUL no convierte
la operación posterior en una copia ilimitada. Los comparadores tampoco pasan
el rango medido a un comparador ilimitado. `strncpy` mantiene padding y semántica
de copia de hasta N bytes.

Se corrigió el mismo overflow de `kstrncpy` en el kernel y en el runtime local
de Vision: con un NUL temprano la implementación anterior podía escribir un
byte adicional después de N. La prueba usa redzones para todas las longitudes
pequeñas, truncación y padding.

La comprobación de free/realloc se realiza dentro del lock del heap. El realloc
que debe moverse fija la allocation original mientras asigna y copia fuera del
lock; no ejecuta reclamación bajo un spinlock externo. Los contadores de pins
rechazan overflow.

## Publicación de objetos GUI

Vision compila parte del código GUI y utiliza el import histórico de ownership
para publicar objetos recién creados. Bloquearlo indiscriminadamente rompía la
creación de ventanas. Ahora sólo el proceso autenticado como servidor Vision
puede publicar una allocation propia con owner=0; no puede tomar una allocation
ajena ni elegir otro owner.

El kernel conserva 512 registros de publicación en BSS supervisor. El publisher
puede liberar o redimensionar sus objetos publicados; que un header tenga
owner=0 no concede esa autoridad a cualquier aplicación. Los registros se
eliminan antes de reutilizar una dirección. Su agotamiento falla sin modificar
la allocation. El cambio de subsistema también requiere propiedad o una
publicación registrada.

## Metadata y lifetime de memoria

Los registros de regiones, fuentes de archivo y chunks del cache del pager
se asignan exclusivamente en frames supervisor. Se eliminó el fallback al heap
accesible desde Ring 3. Bajo presión se recuperan páginas de datos antes de
consumir la reserva de 16 frames usada para nuevas tablas de páginas. Esto evita
que la nueva metadata residente haga fallar fork por falta de tablas cuando
hay datos que pueden paginarse. La reserva es una política de reclamación, no
una partición física dedicada.

Cada copia de usuario conserva una referencia al espacio privado durante el
pinning y la copia. Las tareas planas usan el directorio permanente del kernel,
que no se destruye. La adquisición falla para un espacio en retiro.

El reaper conserva todos los recursos de un prestamista mientras existan
prestatarios. Antes de liberar el último thread, intenta retirar el espacio:
no admite CPUs activas, referencias de copia ni prestatarios. Una vez retirado,
no se permiten nuevas adquisiciones o préstamos. La destrucción repite las
comprobaciones bajo el mismo lock que utiliza el cambio de CR3. El reaper puede
seguir con otros zombies mientras espera. Las referencias prestadas siguen al
objeto cuando exec intercambia identificadores de proceso.

Las tareas nativas arrancan con IOPL=0, y la normalización de sus frames elimina
los bits IOPL. El acceso a puertos y las operaciones privilegiadas quedan en los
entry points del kernel.

## Verificación

Se compilaron kernel, Stage 2 y Vision. El kernel sigue dentro del límite de
build. Las 23 pruebas host seleccionadas ejecutan código de producción con
mocks; los resultados completos y los hashes están en
`tests/kernel/evidence/hardening-2026-10-13/result.json` y `host-tests.json`.

Pruebas nuevas o ampliadas:

```sh
python3 tests/kernel/audit_native_api_boundary.py
python3 tests/kernel/test_address_space_lifetime.py
python3 tests/kernel/test_usercopy_lifetime.py
python3 tests/kernel/test_heap_realloc.py
python3 tests/kernel/test_string_bounds.py
python3 tests/kernel/test_swap_pager.py
```

El antiguo diagnóstico de escritura insegura ahora exige rechazo y comprueba
que el objeto simulado del kernel permanezca intacto. También prueba accesos
válidos, aliases, fallos de pinning, memoria readonly, metadata, free ajeno y
publicaciones autenticadas. El test de lifetime verifica prestatarios, CPUs
activas, referencias de copia, saturación, ciclos corruptos y cambios de IDs.

Las ejecuciones QEMU usan copias aisladas de la imagen habitual:

```sh
python3 tests/kernel/run_ram_smoke.py --ram 64 --smp 2 --smoke \
  --fixture hardening --out tests/kernel/evidence/hardening-2026-10-13/final-security
python3 tests/kernel/run_ram_smoke.py --ram 32 --smp 2 --smoke \
  --fixture ram --out tests/kernel/evidence/hardening-2026-10-13/final-desktop
python3 tests/kernel/run_ram_smoke.py --ram 32 --smp 2 --smoke \
  --fixture swap --swap-file --out tests/kernel/evidence/hardening-2026-10-13/final-swap
```

La prueba de hardening opera desde Ring 3 real: intenta leer/escribir texto del
kernel, escribir BSS y headers del heap, copiar strings al kernel, cambiar owner
sin autorización y liberar un puntero inválido. También verifica IOPL, realloc
y la identidad `used + free + reserved == total`. La prueba de escritorio cubre
ventanas, modos de color, aplicaciones y 256 ciclos de gráficos Linux; la de
swap verifica integridad, fork, aislamiento y liberación bajo presión.

## Límites que permanecen

- El heap heredado sigue mapeado como usuario y muchos payloads compartidos
  tienen owner=0. Esta etapa protege metadata del pager y operaciones concretas
  de la API, pero no impide todos los stores directos de CPL3 al heap compartido.
  Hay que migrar imágenes, stacks, TLS, allocations y vistas GUI a mappings
  privados o compartidos explícitos para completar ese aislamiento.
- Otras APIs todavía pasan punteros directos: GUI, archivos, formatos variádicos
  y otros helpers necesitan validación de firma y objetos, o handles. No se
  certifica toda la API por haber cerrado estas primitivas.
- No se añadieron exception fixups generales como los de Linux. Tampoco se
  certifican todas las carreras SMP, drivers físicos ni Wine completo con estas
  pruebas de host y QEMU.
