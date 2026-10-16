# BFS4: transacciones de metadatos y recuperación

BFS4 conserva la versión de disco `0x00020000`, sus inodos de 256 bytes y bloques de 4096 bytes. Las imágenes anteriores, con la reserva libre a cero, se montan sin convertir ni mover archivos.

El registro de deshacer utiliza los sectores 3840–4081 de la reserva de arranque de cada volumen. Queda fuera del MBR, Stage 2, la reserva de kernel de 1280 KiB y la reserva de 3584 sectores del medio de fuentes. No utiliza bloques libres ni el archivo de swap. Contenidos desconocidos en esa reserva impiden el montaje; nunca se sobrescriben suponiendo que son espacio libre.

Cada operación registra la imagen anterior de hasta 120 sectores diferentes de metadatos. Las cabeceras alternas, descriptores y datos llevan CRC; los descriptores también identifican la transacción y el destino. El controlador persiste los registros antes de publicar su cantidad, y publica la cantidad antes de sobrescribir los metadatos. La confirmación es la última escritura. ATA/AHCI ya completaban escrituras con flush; USB MSC incorpora SYNCHRONIZE CACHE(10) y propaga su fallo.

Un corte deja una transacción activa. El montaje valida todos sus registros y límites antes de restaurar sectores, en orden inverso, y limpiar el registro. Puede repetir una recuperación interrumpida. Un registro dañado o pendiente en un dispositivo de solo lectura impide el montaje. El kernel no libera bloques huérfanos ni trunca archivos por conjetura.

Creación, crecimiento, truncado, borrado, reemplazo por rename, permisos, tiempos y sync comparten transacciones. La desfragmentación mantiene abierto el registro de un archivo durante sus pasos acotados de copia. Un escritor cancela esa reserva antes de iniciar su propia transacción. El mutex recursivo de BFS4 cubre también lectores, comprobación, montaje, reparación y llamadas internas del kernel, independientemente del dominio VFS.

Si una operación falla, el controlador intenta recuperar el registro y recargar los metadatos en RAM. Si eso falla, bloquea nuevas mutaciones y el montaje necesita una recuperación válida. El límite del registro provoca un error recuperable; no permite una escritura sin protección. Las escrituras de datos sobre un archivo existente pueden ser parciales ante un fallo de E/S: el registro protege la estructura y la asignación, no ofrece reemplazo atómico del contenido completo del archivo.

El chequeador comprueba CRC e identidad de inodos, tipos, padres, ciclos, nombres duplicados, tamaños, extents, tablas indirectas, referencias, bloques reservados y contadores. Antes de marcar CLEAN se requiere una comprobación válida. Una reparación explícita solo corrige un contador libre cuya inconsistencia sea la única encontrada; nunca aplica la reparación FAT a un volumen BFS4. Al truncar se libera una tabla indirecta que ya no hace falta.

## Herramientas del host

Con QEMU detenido, para una copia de una imagen con transacción pendiente:

```sh
python3 tools/bfs4.py recover copia.img
python3 tools/bfs4.py check copia.img
```

Para una imagen con el volumen a partir del sector 2048:

```sh
python3 tools/bfs4.py recover copia-usb.img --boot-sectors 6144
```

La recuperación valida el registro completo antes de escribir y sincroniza el archivo. Rechaza imágenes abiertas por QEMU. El chequeador, extractor y editor rechazan transacciones pendientes. La actualización del kernel conserva la reserva del registro.

Antes de abrir una imagen con un kernel o herramienta antiguos, se debe recuperar y comprobar con las herramientas actuales. Los binarios antiguos no conocen el registro y no pueden completar una transacción interrumpida.

## Regresiones

- `tests/storage/test_journal.py`: cortes en cada escritura de diez operaciones, incluido rename con reemplazo y desfragmentación; montaje, integridad estructural y archivo ajeno intacto.
- `tests/storage/test_smp.py`: ocho llamadores concurrentes sin el lock VFS, 320 ciclos de creación/rename/lectura/borrado, comprobación y recuperación exacta de espacio.
- `tests/storage/test_recovery.py`: contador DIRTY, montaje de solo lectura, huérfanos rechazados, errores de escritura y bits reservados.
- `tests/storage/test_defrag.py`: copia acotada, cancelación, escritores concurrentes, fallos de datos y publicación, tablas indirectas.
- `tests/storage/test_swap_file.py`: firma, offsets de E/S, pin de swap y exclusión de cambios y desfragmentación.
- `tests/userland/bfs4_smoke.c`: operaciones a través de VFS dentro de QEMU, eliminación completa del árbol y chequeador final. El espacio global puede variar por las escrituras de servicios; el test de host sin otros escritores verifica su devolución exacta.

Los cortes son inyecciones reproducibles en un disco temporal, no pruebas de desconexión eléctrica de hardware físico. La durabilidad exige que el dispositivo respete la terminación de flush.
