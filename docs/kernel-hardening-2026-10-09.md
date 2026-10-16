# Primera etapa de refuerzo del kernel — 2026-10-09

Esta etapa corrige problemas concretos de memoria y permisos. No convierte el
sistema en un kernel NT ni completa el aislamiento de las aplicaciones nativas.

## Referencias estudiadas

Se consultó el árbol local de ReactOS, sin incorporar su código:

- `reactos-master/ntoskrnl/mm/ARM3/mminit.c`, `MiScanMemoryDescriptors`:
  distinguir memoria física, regiones reservadas y páginas disponibles.
- `reactos-master/ntoskrnl/mm/ARM3/pfnlist.c`: actualizar disponibilidad cuando
  una página cambia entre libre y ocupada.
- `reactos-master/ntoskrnl/mm/ARM3/virtual.c`, `MiProtectVirtualMemory`:
  comprobar propiedad y extensión de una región antes de cambiar sus permisos,
  bajo el bloqueo del espacio de memoria.

BlesKernOS conserva sus tablas de procesos, bitmap de frames y ABI. Se aplicaron
estos principios a sus estructuras existentes.

## Cambios

- La RAM usada incluye reservas fuera de los pools, payloads ocupados y metadata
  del heap, además de frames VM realmente ocupados. Los frames libres del pool
  VM ya no aparecen usados. Los bits permanentes del hueco Win32 se cuentan una
  sola vez. La RAM total sigue siendo la RAM usable reportada por E820, que puede
  diferir de la cantidad instalada/configurada.
- `mm_get_info()` cuenta las cabeceras del heap como usadas; su campo `free_bytes`
  representa payload libre. Los snapshots del heap y del pool se toman bajo sus
  respectivos locks, pero no forman una transacción única entre ambos allocators.
- Paging rechaza desbordamientos del rango y de su redondeo a páginas. Un cambio
  de protección con dirección desalineada incluye todas las páginas tocadas.
- `paging_process_access()` requiere PTEs privadas de usuario en todo el rango.
  La presencia de una tabla privada no permite modificar permisos de sus PTEs
  heredadas del kernel. `unmap` también requiere propiedad privada.
- La validación de acceso de usuario respeta los permisos efectivos PDE y PTE.
- Los remapeos exactos preparan todas sus tablas antes de escribir PTEs. Un fallo
  de asignación conserva las PTEs previas. Las tablas privadas vacías creadas
  durante la preparación se retienen hasta destruir el espacio y pueden
  reutilizarse.
- Los remapeos publican una generación TLB y solicitan invalidación remota.
  El filtro incluye procesos que comparten CR3 mediante préstamo/CLONE_VM.
  Cambiar de proceso con el mismo CR3 pero generación obsoleta recarga CR3;
  una edición concurrente posterior no se marca como ya observada.
- El administrador de procesos muestra RAM usada, total, libre y porcentaje.
- `mem [-b|-k|-m]` muestra unidades explícitas, porcentaje y reserva reportada.
  `top [-n muestras] [-d intervalo_ms]` permite muestreo periódico acotado.
  Ambos aceptan `--help`; el comando `help` describe las opciones.
- La prueba Ring 3 rechaza lectura por syscall de direcciones nula, texto del
  kernel y BSS del kernel; comprueba además `used + free == total`.

Ejemplos de terminal:

```text
mem -m
mem -b
top -n 10 -d 1000
```

## Verificación

```sh
python3 tests/kernel/test_memory_accounting.py
python3 tests/kernel/test_paging_map.py
python3 tests/kernel/test_paging_tlb.py
python3 tests/desktop/test_vm_protect.py
python3 tests/desktop/test_vm_auto_reservations.py
make -j4 build/kernel.bin build/stage2.bin build/programs/processmanager.o \
  build/programs/ring3proxy.o build/system/commands/mem.o \
  build/system/commands/top.o build/system/commands/help.o
```

Las pruebas host ejecutan funciones del código de producción con dependencias
simuladas. Cubren reservas, asignación/liberación de frames, saturación, OOM,
permisos, overflow, rangos desalineados y coherencia de generaciones TLB.
La simulación de CR3 no reemplaza una prueba de carreras reales entre CPUs.

Se arrancó una copia temporal del disco en QEMU TCG, con 64 MiB/1 CPU,
256 MiB/2 CPUs y 1024 MiB/4 CPUs. La imagen contiene kernel, Stage 2,
Process Manager, comandos y la prueba Ring 3 actualizados. Cada ejecución usa
`snapshot=on`; las imágenes habituales no se modificaron.

Los logs y el hash del kernel están en
`tests/kernel/evidence/hardening-2026-10-09/result.json`.
La imagen para probar es `build/kernel-hardening-test.img`.

## Límites y próximos trabajos

- El heap de aplicaciones nativas y la vista PE fija siguen compartidos y
  accesibles desde Ring 3. Separarlos exige migrar código, stacks, TLS y
  allocations a mappings privados y revisar APIs que pasan punteros directos.
- Las filas de memoria por proceso todavía usan allocations del heap y hints;
  falta contabilizar sus frames VM y páginas compartidas con una definición
  explícita de memoria residente frente a privada.
- Los timeouts de shootdown todavía registran un aviso y permiten continuar.
  Antes de garantizar liberación segura de frames bajo cualquier carga SMP,
  hay que impedir reutilizarlos mientras alguna CPU conserve traducciones.
- Falta reforzar lifetime/referencias de espacios prestados y las carreras entre
  destrucción del espacio, cambio de CR3 y copias de usuario.
- El allocator todavía utiliza una región contigua de E820 y un bitmap de runs
  contiguos. No administra toda RAM fragmentada ni implementa swap/demand paging.
- El arranque hasta Vision y la prueba Ring 3 no certifican Wine ni todos los
  drivers. La prueba de 256 MiB muestra agotamiento de frames durante fork de
  Wine; en las pruebas cortas de 256/1024 MiB el arranque de Wine sigue esperando.
  El worker screensaverd también registra una entrada Ring 3 no accesible.
- No se realizó una prueba visual interactiva del administrador de procesos o
  ejecución manual de los nuevos argumentos de terminal dentro del guest.
