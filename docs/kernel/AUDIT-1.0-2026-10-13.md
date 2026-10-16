# BlesKernOS 1.0: informe inicial de auditoría

Fecha: 2026-10-13. Alcance: inspección inicial del árbol de producción,
reproducciones host del pager y comparación de mecanismos concretos.
No es una certificación de seguridad ni una auditoría exhaustiva de todos los drivers.
El directorio conserva el nombre 0.9; el diagnóstico de panic identifica 1.0/BK1.
No hay repositorio Git en este workspace. Se conservan hashes iniciales en
`tests/kernel/evidence/audit-2026-10-13/baseline-sha256.txt` y un parche de esta etapa.

## Estado observado

| Subsistema | Implementación observada | Límite que impide considerarlo completo |
|---|---|---|
| Paging/VM | x86 de 32 bits, páginas de 4 KiB y PDE grandes; CR0.PG/WP; página cero protegida; tablas privadas bajo demanda; generación TLB y máscara de CPUs que usan el espacio | Heap histórico publicado a usuario; destrucción y préstamos de CR3 necesitan garantías de lifetime |
| Pager/swap | Regiones anónimas y de archivo, carga diferida, COW, pins, cache dispersa y swap; mutex del pager y dominio VFS | Pinning no cubre allocations históricas; rollback y teardown bajo fallo SMP requieren pruebas adicionales |
| Ring 0/3 | Tareas de usuario, stacks separados, TSS y syscalls; thunks nativos hacia API del kernel | Funciones GUI/render/API siguen ejecutándose en la pila kernel del llamante |
| Scheduler/SMP | Runqueues por CPU, claims atómicos, afinidad, IPIs, locks y watchdog | `scheduler_smp_cpu_allowed()` fija todos los threads Linux a CPU1 cuando hay varios CPUs: restricción explícita de estabilidad |
| Syscalls | Límites de tamaño, tablas de recursos/FD por proceso, helpers usercopy y comprobación de PDE/PTE | API genérica copia palabras, sin esquema de tipos/longitudes para punteros; comprobación seguida de memcpy no equivale a copia recuperable |
| VFS/BFS | VFS para FAT/ISO y adaptación BFS4, cache de archivos y referencias de mappings; BFS4 con journal, cache bajo spinlock y serialización | Persistencia depende de propagación de errores y de flush real del dispositivo; debe probarse por backend |
| Diagnóstico | Logs seriales, excepciones por proceso, guard de API, watchdog y pantalla de panic con CPU/PID/locks | Panic usa consola, gráficos y sonido; estas dependencias pueden estar bloqueadas o corruptas |

Fuentes locales principales: `kernel/mm/{paging,pager,memory,usercopy,swap}.c`,
`kernel/sched/{task,scheduler_smp,klock,smp_watchdog}.c`,
`kernel/arch/x86/{idt,gdt}.c`, `kernel/api/syscall.c`,
`kernel/exec/elf_loader.c`, `kernel/drivers/core/vfs.c`,
`kernel/drivers/storage/{bfs4.c,bfs4_journal.inc}`, `kernel/core/panic.c`.
Las notas del 9 de octubre no describen todo el estado actual: hoy sí existen
pager y swap. Sus observaciones de compatibilidad sirven como antecedentes,
no como resultados de esta ejecución.

## Hallazgos y prioridad

Gravedad: crítica = posible corrupción/violación de privilegios; alta = pérdida
de disponibilidad o integridad; media = funcionamiento incorrecto acotado.
Reproducibilidad distingue prueba ejecutada, evidencia estática y escenario pendiente.
Riesgo de cambio considera ABI, persistencia y concurrencia.

| ID | Gravedad / prioridad | Evidencia y reproducción | Riesgo de corregir |
|---|---|---|---|
| VM-01 | Crítica / P0 | Heap compartido: `mm_init()` invoca `paging_configure_user_layout(mm_heap_start(),mm_heap_end())`. `sys_vm_maps_reserve()` usa `krealloc` para tablas VM; ownership 0 no es protección hardware. Existe guard de la página raíz del allocator, pero no aislamiento de todo el heap. Reproducción adversarial guest pendiente | Alto: migración de ELF, stacks, TLS, SYS_ALLOC y API |
| API-01 | Crítica / P0 | `SYS_API_CALL` copia hasta 16 palabras; `elf_user_api_dispatch()` termina en `elf_api_call_raw()` en Ring 0. Validar token y stack no valida los objetos apuntados por argumentos. Exploit concreto pendiente | Alto: wrappers tipados y ABI legado |
| VM-02 | Crítica potencial / P0 | `syscall_process_cleanup()` descarta resultados de unmap y libera backing; `pager_destroy()` libera frames sin comprobar pins; `paging_process_destroy()` libera tablas sin protocolo local de retirada. Una CPU con traducción o referencia pendiente podría alcanzar memoria reutilizada. Escenario SMP pendiente; hay guards del reaper que deben analizarse antes de concluir alcanzabilidad | Alto: coordinar borrowers, reaper, CR3 y shootdown |
| VM-03 | Alta / P1 | `pager_unmap(base+1,4096)` revoca la traducción y después `paging_process_unmap()` rechaza alineación. Prueba host falla por mutación antes del rechazo | Bajo: validación previa sin cambiar ABI |
| VM-04 | Alta / P1 | `pager_pin(1,0xfffff000,1,false)` con mapping incrementa pin, devuelve false y no lo deshace. Sin mapping, `continue` evita el guard de overflow y el loop vuelve a VA cero; el caller de usercopy aún no validó el puntero. Reproducción host de pin y de iteración fuera del rango; bloqueo guest adversarial pendiente | Bajo: iteración acotada por última página |
| COPY-01 | Alta potencial / P1 | `copy_from/to_user()` pin + comprobación + `kmemcpy`; pins del pager no protegen mappings legacy, y cambios de protección/lifetime pueden competir con la copia. No hay fixup específico en estos helpers. Carrera guest pendiente | Medio/alto: protocolo VM y recuperación de faults |
| SMP-01 | Alta potencial / P1 | Threads Linux limitados a CPU1 por bookkeeping compartido. Afinidad reduce paralelismo pero no prueba ausencia de carreras frente a tareas nativas/cleanup. Pruebas de carga pendientes | Alto: mantener afinidad hasta verificar VM/signals |
| LOCK-01 | Media / P2 | Profundidad recursiva de spin/mutex/rw satura en 0xffff pero los unlock decrementan: una adquisición adicional deja de contarse. Evidencia estática; no caso real reproducido | Medio: assert/fail-stop y revisión de callers |
| VM-05 | Limitación arquitectónica / P2 | Protección representada por bits user/writable en paging de 32 bits sin NX; `PROT_EXEC` no aporta barrera hardware de ejecución independiente | Alto: requiere evaluar PAE/NX y hardware legado |
| IO-01 | Investigación / P1 | BFS4 tiene journal y estado para error de escritura; VFS mantiene caches y mappings compartidos. No se afirma pérdida de datos nueva sin inyección de fallos. Revisar write/close/sync y flush ATA/AHCI/USB | Alto en disco: solo imágenes desechables |
| DIAG-01 | Alta potencial / P2 | `panic_show()` llama kprintf, domains, gráficos y sonido; consola tiene locks. Riesgo de bloqueo en panic si una CPU retiene recursos. Inyección pendiente | Medio: salida serial mínima sin locks y parada coordinada |

VM-03 y VM-04 se reprodujeron antes de modificar producción con
`tests/kernel/test_pager_ranges.py`; logs `unmap-before.log` y `pin-before.log`.
El harness ejecuta funciones extraídas del código actual con dependencias
simuladas y verifica efectos, no solo presencia de texto. No modela carreras CPU.
Una prueba adicional `gap` contra la copia inicial detecta wrap a VA cero en
el caso sin mapping; `gap-before.log` conserva ese resultado. El mock corta la
iteración fuera del rango para no dejar una prueba host colgada.

## Comparaciones técnicas consultadas

- Linux define que un flush de rango debe eliminar las traducciones previas en
  las CPUs pertinentes. La máscara por espacio es una optimización válida; un
  timeout no acredita que un frame sea reutilizable. Aplicación propuesta:
  retirada diferida o cuarentena con confirmación, antes de liberar backing.
  [Cache/TLB](https://docs.kernel.org/core-api/cachetlb.html).
- FreeBSD, NetBSD y OpenBSD ofrecen copias kernel/usuario con retorno EFAULT;
  las copias de strings son acotadas. Aplicación: wrappers de API con esquema
  de argumentos y copia recuperable, conservando los tokens y firmas nativas.
  [FreeBSD copy(9)](https://github.com/freebsd/freebsd-src/blob/main/share/man/man9/copy.9),
  [NetBSD copy(9)](https://man.netbsd.org/copy.9),
  [OpenBSD copyin(9)](https://man.openbsd.org/copyin.9).
- SerenityOS comprueba overflow al redondear direcciones y distingue faults
  en IRQ/zonas protegidas en `MemoryManager`. Aplicación: límites comprobados
  antes de efectos y diagnósticos según contexto.
  [MemoryManager.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/MemoryManager.cpp).
- Haiku usa referencias/bloqueos de address space al fijar memoria y deshace
  páginas fijadas si falla. Sus entradas `_user_create_area/_user_map_file`
  filtran modos kernel, rangos y copian argumentos/resultados con comprobación.
  Aplicación: pins equilibrados, propiedad explícita y rollback verificable.
  [vm.cpp](https://github.com/haiku/haiku/blob/master/src/system/kernel/vm/vm.cpp).

Estas referencias son patrones de contrato y ownership, no una propuesta de
importar sus arquitecturas ni código. Las URLs de ramas móviles se consultaron
en la fecha del informe; no se fijó un commit upstream.

## Etapas de implementación y criterios de salida

1. **Rangos del pager:** reproducir VM-03/04, corregir separadamente, probar
   balance de pins, cruces de página, fallo de carga y rechazo sin mutaciones.
2. **Lifetime VM/SMP:** fixture con threads que copian/unmap/protect mientras
   muere el propietario, CLONE_VM y vfork; forzar ACK fallido. Nunca liberar
   frames/tablas mientras existan traducciones o referencias activas. Mantener
   frames en cuarentena si no se puede demostrar retirada segura.
3. **API nativa y Ring 3:** inventario de exports y firmas; wrappers de copia
   para strings/buffers y handles para objetos kernel. Migrar primero utilidades
   puras/render CPU; después memoria ELF/stacks/TLS/SYS_ALLOC a espacios privados.
   Mantener Vision como servicio, trasladar cómputo GUI fuera de Ring 0 por grupos
   de API y conservar en kernel drivers y operaciones privilegiadas necesarias.
   Retirar PAGE_USER del heap solo cuando todos los consumidores estén migrados.
4. **Scheduler/IRQ/locks:** reproducir handoffs sleep/wakeup/exit, orden de locks,
   hooks IF/preemption y contexto FPU/TSS. No levantar afinidad Linux por rendimiento
   hasta pasar carga sostenida de Wine con múltiples CPUs.
5. **VFS/BFS/E/S:** corte de energía por sector, fallos read/write/flush, close/sync,
   coherencia MAP_SHARED/cache e integridad después de remontar. Probar cada backend.
6. **Diagnóstico:** serial de emergencia sin dependencias bloqueantes, registro
   CR2/EIP/CR3/CPU/PID, watchdog y pruebas de panic mientras hay locks retenidos.

Cada etapa conserva pruebas y hashes, documenta límites y admite revertir el
parche. Matriz mínima guest: 1/2/4 CPUs, RAM baja y 256/1024 MiB, apps nativas,
Vision, Wine, swap y BFS4; ampliar a drivers físicos antes de release.
Compilar no certifica compatibilidad de Wine ni soporte de hardware.
Resultados de la primera etapa: `AUDIT-1.0-STAGE1-2026-10-13.md`.

## Continuación: estado posterior al informe inicial

La [etapa 2](AUDIT-1.0-STAGE2-2026-10-13.md) mueve la tabla dinámica de
mappings a frames supervisor y corrige ACKs TLB al desbordarse la generación
de 32 bits. Incluye nuevas reproducciones, 24 pruebas host y tres ejecuciones
QEMU del kernel final. Las referencias upstream ahora se conservan por commit
y SHA-256. VM-01 sigue abierto para el resto del heap; API-01 y VM-02 no quedan
resueltos. También se reprodujo agotamiento del heap al arrancar Vision en
16 MiB tanto antes como después de la etapa 2.
