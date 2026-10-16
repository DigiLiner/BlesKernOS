# Auditoría 1.0: etapa 2, metadatos supervisor y generaciones TLB

Esta continuación reduce la exposición de la tabla VM de los procesos y corrige
el orden de ACKs al desbordarse el contador TLB. Conserva arquitectura x86 de
32 bits, ABI nativa/Linux, afinidad Linux, scheduler y formatos de almacenamiento.
No completa el aislamiento ni la retirada concurrente de espacios de memoria.

## Código de referencia consultado

Se descargaron archivos concretos de repositorios oficiales, fijando el último
commit que modificó cada archivo y su SHA-256. Las copias completas conservan
sus cabeceras/licencias en `tests/kernel/evidence/audit-stage2-2026-10-13/upstream/`.
No se incorporó código upstream al kernel. El inventario exacto es `references.json`.

| Referencia | Mecanismo observado | Aplicación a BlesKernOS |
|---|---|---|
| [SerenityOS MemoryManager](https://github.com/SerenityOS/serenity/blob/fb23f3c90282f916a74a062ba6a6d4e8c9e884c0/Kernel/Memory/MemoryManager.cpp) | `allocate_kernel_region()` coloca regiones en el espacio kernel; `protect_kernel_image()` quita escritura de texto/rodata y aplica NX cuando existe | Separar metadatos de memoria publicada a usuario; revisar permisos de texto/rodata como etapa posterior |
| [FreeBSD vm_map](https://github.com/freebsd/freebsd-src/blob/8eef59db254283eaaee47be94d4ea9f2fef4f2ad/sys/vm/vm_map.c) | `vm_map_startup()` configura zonas para entradas de mapas y vmspaces; los objetos de control no son buffers del usuario | Allocation específica de descriptores y lifetime separado de las allocations de aplicaciones |
| [Linux mmu_gather](https://github.com/torvalds/linux/blob/1fb3d8c20bfadbbe2d9e5de18074de9282a52b5f/mm/mmu_gather.c) | `tlb_flush_mmu()` invalida antes de liberar lotes de páginas/tablas | La liberación debe depender de retirada confirmada, no de haber escrito una PTE |
| [Linux jiffies](https://github.com/torvalds/linux/blob/f6472b17933f9adb825e7c7da31f7b7b2edb1950/include/linux/jiffies.h) | Comparaciones mediante diferencia con signo soportan wrap; los contadores de 64 bits requieren lecturas coordinadas en plataformas de 32 bits | Corregir la comparación TLB sin introducir lecturas partidas de un uint64_t |
| [Haiku VM](https://github.com/haiku/haiku/blob/d0bea294e76bb48c10753b199faf0ec2683f0cc4/src/system/kernel/vm/vm.cpp), `lock_memory_etc/unlock_memory_etc` | Se retiene referencia al address space, se bloquean áreas/cache y se deshace wiring parcial si falla | Diseño de referencia para pendiente lifetime/pins; no se ha importado su implementación |

El enlace exacto de Haiku y el de FreeBSD se generan/verifican contra el
inventario fijado en `references.json`; las ramas móviles no definen esta evidencia.

## Hallazgos y cambios

### VM-01a: tabla de mappings en heap accesible a Ring 3

Gravedad **crítica**, prioridad **P0**, reproducción host determinista; riesgo
de cambio medio por consumo del pool VM y rutas de crecimiento/fork/cleanup.

`sys_vm_maps_reserve()` usaba `krealloc` del heap histórico. Sus descriptores
contienen backing físico, permisos, flags y referencias compartidas. El tag
`owner_process_id=0` controla lifetime del allocator, no permisos PDE/PTE.
La estructura VM exterior estaba en BSS supervisor, pero `vm.maps` apuntaba al
heap de usuario. La prueba original falla al verificar la clase de allocation.

Ahora las tablas se reservan con `mm_vm_frame_alloc()`, fuera del heap publicado
a usuario. El guard existente `mm_kernel_identity_overlaps()` también impide
que los syscalls de mappings sombren el pool físico que CPL0 necesita.
Se copian entradas, se inicializan los nuevos slots, se publica la tabla y se
devuelven los frames antiguos. OOM conserva puntero, capacidad y contenido.
`sys_vm_maps_release()` sustituye `kfree` en cleanup. No hay fallback al heap.

El límite sigue siendo 1024 slots y la capacidad inicial 64. En i386 los
descriptores ocupan 24 bytes: una tabla inicial usa 1 página y una de 1024 usa
6 páginas. Durante crecimiento coexisten temporalmente las dos tablas; puede
fallar si faltan frames contiguos aunque exista heap libre. La RAM usada global
cuenta estos frames en VM, ya no como payload de heap. No se agregó atribución
de estos frames a la fila de memoria de cada proceso.

La protección cubre el acceso directo por Ring 3 al alias físico; no impide que
una API genérica mal validada en Ring 0 escriba allí. FD tables, LDT, metadata
de allocations y otras estructuras aún usan heap histórico. VM-01/API-01 del
informe inicial siguen abiertos.

### VM-06: ACK antiguo aceptado cuando la generación TLB vuelve a cero

Gravedad **alta**, prioridad **P1**, reproducción host determinista; requiere
wrap del contador de 32 bits en funcionamiento real. Riesgo de cambio bajo.

`paging_wait_generation()` comparaba `seen < generation`. Con `seen=0xfffffffe`
y `generation=0`, la CPU remota se consideraba actualizada sin invalidar.
Ese falso ACK puede permitir reutilizar backing que todavía tiene una traducción
remota. La nueva prueba falla sobre la función original antes del cambio.

La comparación usa diferencia modular con signo de 32 bits; se aceptan ACK
actual o posterior y se rechaza el anterior a través del wrap. La ordenación
requiere distancia menor a 2^31 publicaciones; no se implementa un protocolo
de epochs sin límite ni se demuestra ese límite frente a una CPU detenida
indefinidamente. El timeout y los problemas de teardown de VM-02 permanecen.

## Pruebas ejecutadas

Después del cambio de tablas se ejecutaron 23 pruebas host y fixtures QEMU.
Después del cambio TLB se repitió la matriz: **24/24 programas host pasan**.
Se conservan resultados antes/después y logs separados por programa.

- `test_vm_metadata.py` ejecuta reserve/release de producción con un pool
  simulado en i386: inicialización, 64/128/256/1024 slots, crecimiento con
  cambio de dirección, OOM sin efectos, tablas independientes y cero frames
  pendientes después de liberar. No demuestra por sí sola un fault Ring 3 real.
- `test_tlb_generation_wrap.py` ejecuta la espera de producción: ACK antiguo,
  actual/posterior, retraso remoto, wrap y máscara vacía/offline. Simula ACKs,
  no reemplaza una carrera SMP en hardware.
- La matriz conserva pruebas de pager/swap, lookup, paging/TLB/OOM, heap,
  accounting, stacks, signals, handoff SMP, ABI i386, protección VM, block/ATA,
  BFS4 recovery, journal, operaciones concurrentes y swap file.

La nueva fixture `--fixture vm` crea 130 mappings para superar capacidades
64/128, usa `mprotect`, clona con `clone(SIGCHLD)`, hace crecer el hijo más allá
de 256 entradas, comprueba independencia de datos, reutiliza huecos y repite
tres rondas. La última sale sin unmap para probar cleanup. El wrapper exige
recuperación de frames y de páginas virtuales.

| Kernel final, QEMU TCG/VGA std | Resultado | Evidencia |
|---|---|---|
| 64 MiB / 1 CPU, fixture VM | PASS | Hijo 0, integridad/crecimiento/fork/reuse PASS, VM libre antes/después 15564800 bytes |
| 1024 MiB / 4 CPUs, fixture VM | PASS | Hijo 0, mismas comprobaciones, VM libre antes/después 712802304 bytes |
| 32 MiB / 2 CPUs, fixture swap | PASS | 10744 escrituras / 8104 lecturas, 0 errores E/S, slots liberados e integridad PASS |

Los JSON finales guardan el SHA-256 del kernel realmente escrito en cada copia
de disco. Las primeras fixtures VM/SMP y swap también pasaron antes del cambio
TLB; se guardan como resultados intermedios, sin atribuirles el kernel final.

La primera versión de la fixture intentó syscall i386 `fork` (2), actualmente
sin traducción, y recibió ENOSYS. Se corrigió para usar `clone(SIGCHLD)` (120),
el mismo camino de fork que usa la fixture swap existente. No se agregó la
syscall faltante como función nueva.

### Resultado negativo: desktop en 16 MiB

La imagen/servicio Vision usados no completaron el arranque en 16 MiB: el heap
presenta OOM repetido para 17152 bytes, con mayor bloque libre de 16144 bytes.
No se registra como PASS ni se certifica esa configuración.
Se ejecutó una comparación con kernel anterior compilado de forma aislada;
su hash coincide exactamente con el de la etapa 1:
`018f28042a78177a8182e5478a6e288761bb64d34fa089dd4a87df41d4ab2de5`.
Los resultados de esa comparación están en `guest-baseline` y `result.json`.
El kernel anterior también terminó sin completar el escritorio, con el mismo
patrón de OOM y fragmentación. En esta combinación de imagen/Vision, el fallo
ya estaba presente antes de ambos cambios; no se atribuye a la nueva tabla VM.

El kernel final ocupa 825388 bytes y su SHA-256 es
`deacc61b18976e37801d9582407c3dbdc91aef81f0a38202a1744207dd399159`.

## Reproducibilidad y alcance

```sh
python3 tests/kernel/test_vm_metadata.py
python3 tests/kernel/test_tlb_generation_wrap.py
make -j4 build/kernel.bin build/stage2.bin
python3 tests/kernel/run_ram_smoke.py --ram 64 --smp 1 --smoke --fixture vm --out /tmp/bk-stage2-vm
python3 tests/kernel/run_ram_smoke.py --ram 32 --smp 2 --smoke --fixture swap --swap --out /tmp/bk-stage2-swap
```

Para reproducir fallos host previos, usar `--source` con `syscall-before.c` o
`paging-before.c` del directorio de evidencia. Deben fallar. `baseline.mk`
permite relink aislado con esos dos archivos y los demás objetos actuales;
en esta ejecución produjo exactamente el hash de la etapa 1. El runner admite
`--kernel-bin/--stage2-bin` para A/B sin sustituir artefactos habituales.

`stage2.patch`, hashes, manifest, fuentes upstream, logs, JSON y capturas están
en `tests/kernel/evidence/audit-stage2-2026-10-13/`. Las imágenes normales no
se modificaron; los discos guest/swap fueron copias desechables. Sí se
recompilaron kernel y Stage 2 de build. No hay Git en este workspace.

No se ejecutó Wine completo: el bundle esperado por la fixture runtime
(`build/phase12-runtime/WINEGATE`) no existe aquí. Las pruebas usan la ABI Linux
que Wine utiliza, pero no certifican compatibilidad de Wine ni drivers físicos.
Sigue activa su restricción a CPU1 en SMP.

## Prioridades siguientes

1. Proteger LDT y tablas FD fuera del heap de usuario. `sys_linux_modify_ldt`
   valida descriptores al copiarlos, pero el backing LDT se reserva con kmalloc
   y la CPU lo consume mediante `gdt_activate_user_ldt`; escritura directa
   posterior es un riesgo crítico pendiente de prueba adversarial guest.
2. Reproducción SMP de cleanup con pins/borrowers activos y shootdown fallido;
   cuarentena de backing antes de liberar/reutilizar. No basta corregir wrap.
3. Wrappers tipados de API nativa y copia recuperable de punteros. Migrar
   buffers/ELF/stacks/TLS antes de retirar PAGE_USER del heap.
4. Recuperación/fragmentación del heap al arrancar Vision con 16 MiB, basada
   en el resultado A/B; no compensar errores habilitando funciones nuevas.
