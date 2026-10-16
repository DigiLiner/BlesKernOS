# Swap en el disco del sistema

BlesKernOS admite `/SYSTEM/SWAP.BKS` en el volumen BFS4 de arranque. Es un
archivo reservado de 32 MiB en la imagen preparada, con 8.191 slots de 4 KiB.
No requiere conectar otro disco a QEMU. Se activa después de montar el volumen.
Si existe un disco de swap dedicado validado, tiene prioridad y el archivo no
se activa simultáneamente.

El archivo tiene que ser regular, completamente asignado, de un único extent,
alineado a 4 KiB y con encabezado BKSWAP01, geometría y CRC32 válidos. El kernel
comprueba el bitmap, referencias y consistencia del volumen antes de fijarlo.
No crea el archivo durante un page fault ni utiliza un archivo cualquiera.

El pager conserva el algoritmo de presión y recuperación de páginas existente.
Después de la activación, la E/S de slots accede directamente al rango de sectores
reservado del archivo, sin VFS, caches de archivo, asignaciones de heap ni locks
del filesystem. Las operaciones del filesystem siguen compartiendo el mutex
del controlador ATA con el swap, pero nunca se invoca VFS desde page-in/out.

Mientras el kernel mantiene el archivo reservado, BFS4 rechaza leerlo como
archivo normal, sobrescribirlo, truncarlo, borrarlo, cambiarle metadatos o
renombrarlo; también impide renombrar sus directorios ancestros. La
desfragmentación lo omite y el volumen no puede desmontarse ni formatearse.
La reserva permanece hasta reiniciar, incluso después de swapoff, para permitir
reactivarlo sin que otro escritor o defragmentador cambie sus bloques. Un
encabezado inválido libera la reserva de activación y deja el archivo intacto.

## Preparación offline

Con QEMU cerrado, y el kernel y Stage 2 compilados:

```sh
python3 tools/install_swap_file.py build/bleskernos-ata-user.img --mib 32 --update-boot
./tools/run_16mb.sh
```

El instalador comprueba BFS4, trabaja sobre una copia temporal y reemplaza la
imagen solamente después de volver a verificarla. Conserva los archivos y sus
permisos. Rechaza reemplazar un SWAP.BKS existente sin firma válida, fragmentado
o de otro tamaño. Ejecutarlo otra vez con el mismo archivo válido conserva sus
extents y datos. No reduce ni amplía automáticamente una reserva existente.

La imagen de esta sesión tiene respaldo `build/bleskernos-ata-user.before-swapfile-*.img`.
El disco de swap separado anterior queda conservado; el lanzador ya no lo conecta.
`run_16mb.sh` configura RAM de 16 MiB y VGA explícita, sin dispositivos agregados
por defecto por QEMU. Esto evita la unidad de CD vacía y su consumo adicional
en equipos con tan poca memoria. El log queda en `build/qemu-16mb.log`.

## Validación

```sh
python3 tests/storage/test_swap_file.py
python3 tests/kernel/test_swap_pager.py
python3 tests/storage/test_recovery.py
python3 tests/storage/test_defrag.py
python3 tests/kernel/run_ram_smoke.py --ram 16 --swap-file --smoke --fixture swap --out /tmp/bk-swapfile16
python3 tests/kernel/run_ram_smoke.py --ram 32 --smp 2 --swap-file --smoke --fixture swap --out /tmp/bk-swapfile-smp
```

El test de almacenamiento compila BFS4 y swap reales; valida rechazo de CRC,
fragmentación y read-only, offset físico de slots, datos, protección contra
modificaciones/desmontaje/formato, swapoff/enable, defrag y conservación de otros
archivos. QEMU comprueba page-outs y recuperación por fallos de página bajo
presión real de 8 MiB, clone privado, permisos, E/S y devolución de slots al salir.
Los resultados se conservan en `tests/kernel/evidence/swapfile-*`.

El backend inicial de archivo requiere BFS4 y ATA. El heap nativo y las otras
reservas residentes no cambian de política por usar un archivo de swap.

## Configuración VMware, KVM, red y audio con poca RAM

La prueba del comando del usuario con Pentium III, KVM, dos CPUs, VMware SVGA,
RTL8139 y AC97 reproduce OOM del heap con 16 MiB. El modo 640×480×8 era
rechazado por VMware y dejaba 800×600×32; el escritorio llegó a tener solo
54 KiB libres de heap. Se corrigió el arranque para intentar 640×480×32
si ese backend no admite 8 bpp; Vision conserva su salida software de 8 bits.
La corrección está aplicada al kernel y la imagen principal.

Incluso con la geometría reducida y sin dispositivos por defecto, esa
combinación completa de drivers/superficies/audio sigue produciendo OOM con
16 MiB. El swap de páginas privadas no sustituye el respaldo físico de esos
bloques del heap. El fixture idle puede marcar PASS aunque existan OOM; para
esta comparación se auditó también el log completo y no se aprobó 16 MiB.

Con los dispositivos originales y **24 MiB** se verificaron arranque, dos
CPUs online y 10 segundos de medición idle, sin OOM. Heap en idle:
16.382 KiB usados / 2.945 KiB libres. Para ese conjunto, cambiar `-m 16` por
`-m 24` conserva los demás argumentos. `run_16mb.sh` sigue siendo la
configuración VGA estándar y dispositivos mínimos probada para 16 MiB.
Evidencia: `tests/kernel/evidence/user-config-memory.json` y sus logs.
