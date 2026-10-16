# Paging y memoria de procesos

## Estado actual

BlesKernOS activa paging IA-32 antes de inicializar el heap, con `CR0.WP` y PSE.
El directorio del kernel mantiene el mapa identidad necesario para framebuffer,
ROM y MMIO; la página cero y el código/datos bajos del kernel son supervisor.
Las ventanas Ring 3 heredadas continúan compartidas mientras sus APIs se migran
de punteros a handles.

Cada proceso dispone ahora de su propio directorio y el scheduler carga su CR3
antes de volver a Ring 3. Las PDE que no cambian comparten las tablas del kernel;
una región ELF fija obtiene tablas privadas de 4 KiB que pueden apuntar a un
backing físico diferente en cada proceso. Al terminar el último hilo se liberan
el directorio y sus tablas. Los cambios en mappings globales se propagan a los
directorios que no hayan privatizado la PDE y conservan el shootdown TLB SMP.

Este diseño ya aísla ejecutables Linux de dirección fija, pero aún es
transicional: el heap nativo y varios buffers GUI siguen compartidos e
identity-mapped. No debe confundirse con aislamiento completo de todo el
espacio de usuario.

## Heap dinámico

El heap comienza después de la imagen del kernel y termina al final de la región E820 utilizable
que lo contiene, alineada a 4 KiB y limitada a 1 GiB. Ya no queda fijado en
64 MiB. Por ejemplo:

- QEMU `-m 64M`: heap de aproximadamente 62 MiB;
- QEMU `-m 128M`: heap de aproximadamente 126 MiB;
- QEMU `-m 256M`: heap de aproximadamente 254 MiB.

Una llamada individual a `SYS_ALLOC` puede solicitar hasta 64 MiB y cada
proceso dispone de 128 registros de asignación. Las APIs nativas que llaman a
`bk_sys_alloc` utilizan el mismo heap sin una cuota artificial por aplicación.

## Liberación al terminar

Cada bloque conserva el `process_id` propietario. Cuando un proceso no tiene
más hilos vivos, el scheduler libera sus stacks, descriptores, sockets, ventanas,
programas GUI, imagen ELF y cualquier bloque que la aplicación no liberó. El
estado zombie y el código de salida se mantienen para que `waitpid` continúe
funcionando, pero ya no retienen la RAM.

La autoprueba `RING3PROXY.O` abandona deliberadamente 96 KiB. El arranque debe
mostrar una línea similar a:

```
[MM] proceso 4: 96 KiB recuperados
```

## ELF Linux fijo

Los `ET_EXEC` i386 se cargan en sus direcciones originales, incluida la base
tradicional cercana a `0x08048000`. El loader escribe y relocaliza a través de
un alias del kernel, publica las páginas en el CR3 del proceso y finalmente
aplica RW o RO/RX según cada `PT_LOAD` (`i486` no tiene NX). La ventana
`0x08000000-0x0A000000` se excluye del allocator para no ocultar memoria viva
del kernel al instalar esos mappings.

La prueba developer `EXECFIX.ELF` inicia dos copias antes de hacer `yield`.
Ambas escriben en la misma dirección virtual y verifican marcadores distintos;
el arranque informa `ET_EXEC dual CR3: OK` cuando el aislamiento funciona.

## Próxima etapa

Queda privatizar el heap y las pilas de todos los procesos, reemplazar punteros
GUI compartidos por handles, e implementar duplicación copy-on-write para un
`fork` real. Después pueden añadirse crecimiento por page fault y swap.
