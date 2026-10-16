# BFS4: sistema de archivos nativo de BlesKernOS

BFS4 es el volumen de datos predeterminado para las imágenes ATA, USB y
VMware. El disquete de arranque y los medios FAT externos conservan FAT12/16/32.
El cargador BIOS ocupa los sectores 0–4095 de la partición; la supermanzana
BFS4 comienza en el sector 4096. En una imagen USB, la partición MBR comienza
en el sector 2048 del disco y tiene tipo `0x83`.

## Formato en disco

- Bloques de 4096 bytes, sectores de 512 bytes, números relativos al comienzo
  de BFS4.
- Bloque 0: supermanzana con geometría, contador de bloques libres, estado
  `CLEAN`/`DIRTY`, etiqueta y CRC32.
- Siguen el bitmap de bloques, el bitmap de inodos y la tabla de inodos.
- Cada inodo ocupa 256 bytes: ID estable, padre, nombre UTF-8 de hasta 127
  bytes, tamaño, modo, tiempos, CRC32, ocho extents directos y un bloque
  indirecto para hasta 512 extents más.
- Los nombres de hijos residen en sus inodos. Al montar se construye un índice
  hash en RAM por `(inodo_padre, nombre)`; las carpetas pequeñas no requieren
  bloques de datos propios. Las búsquedas ignoran mayúsculas ASCII.

El controlador carga bitmaps e inodos en RAM, agrupa transferencias físicas
contiguas en lotes de hasta 124 KiB y mantiene una caché de lectura de bloques
de 2 u 8 MiB según la memoria disponible. Las escrituras de datos y metadatos
son síncronas; el VFS conserva su caché coherente por archivo abierto. Append
reescribe sólo el bloque parcial de borde y los bloques nuevos.

Al comenzar una mutación, BFS4 persiste `DIRTY`. Un apagado o reinicio normal
vacía los archivos abiertos y marca `CLEAN`. Si el siguiente montaje encuentra
`DIRTY`, comprueba inodos, padres, extents, bitmaps y espacio libre. Si detecta
una inconsistencia, rechaza el montaje para evitar nuevas escrituras. BFS4 no
tiene todavía journal ni reparación automática; para recuperar datos de un
volumen dañado se debe trabajar sobre una copia de la imagen.

## Construcción y comprobación

```sh
make build/bleskernos-ata-user.img
python3 tools/bfs4.py check build/bleskernos-ata-user.img
make build/bleskernos-usb-user.img
python3 tools/bfs4.py check build/bleskernos-usb-user.img --boot-sectors 6144
```

`tools/bfs4.py extract IMAGEN DESTINO` extrae la raíz para construir los
instaladores CD y disquete. `tools/bfs4.py pack IMAGEN DIRECTORIO` empaqueta
un árbol en una imagen ya creada con el tamaño solicitado. La ruta heredada
`tools/build_fat32_ata.py` genera BFS4 de forma predeterminada; FAT32 queda
disponible con `BLES_FS=fat32` al invocar el script directamente.

Prueba del controlador real en el host, incluyendo formato, lectura/escritura,
append, renombrado, rechazo de ciclos de directorio sin perder el destino,
eliminación, consistencia y 13 extents fragmentados:

```sh
gcc -m32 -O2 -Ikernel/include kernel/drivers/storage/bfs4.c \
    tests/storage/bfs4_host.c -o /tmp/bfs4_host_test
truncate -s 16777216 /tmp/bfs4-test.img
/tmp/bfs4_host_test /tmp/bfs4-test.img --format --keep-frag
python3 tools/bfs4.py check /tmp/bfs4-test.img
```

El VFS actual limita las rutas a 260 bytes y los tamaños expuestos al ABI
histórico a 32 bits. El formato de inodo guarda 64 bits para el tamaño, pero
ese límite del VFS sigue vigente.

La prueba de integración en KVM arrancó Vision desde ATA y USB BFS4. En ATA,
la asociación normal de `.exe` lanzó Notepad bajo Wine, recibió teclado y
guardó `bfstest.txt` con el contenido `bfs ok`; el volumen pasó `check` tras
ese arranque y también pudo remontarse desde estado `DIRTY`.
