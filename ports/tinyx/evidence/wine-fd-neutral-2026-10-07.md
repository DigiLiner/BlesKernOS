# Wine: cache privado sin FDs y barrera de arranque, 2026-10-07

La imagen ATA conserva los archivos, permisos y configuración de la imagen
anterior. El escritorio permanece bajo el splash «CARGANDO WINE» hasta que
el launcher informa que TinyX, el prefijo, wineserver y `cmd /c exit` están
listos. El handshake usa el PID del launcher, no un marcador persistente.
Un fallo explícito o el watchdog de 180 s libera el escritorio con diagnóstico
serial. Los perfiles de memoria reducida, instalación y recuperación mantienen
sus excepciones de arranque.

## Mediciones

QEMU/KVM, `-cpu pentium3 -smp 2 -m 1536 -vga vmware`, con `media/installers/OFF97.iso`
como CD-ROM. Se usó display sin ventana para capturar las pruebas. Los tiempos
son del reloj monotónico del invitado; cada variante se midió una vez y puede
variar con la carga del host. El prefijo ya estaba preparado. El tiempo de
Wine empieza cuando el kernel lanza el warmup; no incluye BIOS ni la carga
anterior del sistema. Notepad se abre inmediatamente después de la barrera.

| Variante | Wine listo, desde spawn del warmup | Notepad, desde su launcher |
| --- | ---: | ---: |
| Cache privado inicial de 128 entradas, IDE PIO por sector | 118,900 ms | No se usa como medición de aplicación |
| Cache de 128 entradas y READ MULTIPLE, IDE | 58,380 ms | 7,800 ms |
| Final, IDE con READ/WRITE MULTIPLE | **53,020 ms** | **6,760 ms** |
| Final, AHCI, misma CPU y RAM | **19,170 ms** | **2,900 ms** |

La primera fila es una variante intermedia de esta implementación, no una
medición del kernel anterior de 512 handles. La prueba inicial que reempaquetó
el prefijo perdió permisos de `/tmp/.wine-0` y falló; fue descartada. Todas las
mediciones de la tabla arrancaron copias del disco original que conservan sus
permisos. No se alcanzó todavía el objetivo de 10 s para preparar Wine.

En el kernel final, el pico fue **61/128 handles VFS**. Al finalizar el warmup:
181 imágenes, 49,893,376 bytes, cero expulsiones y cero FDs de writeback
retenidos por el cache privado. Con Notepad: 183 imágenes, 51,785,728 bytes.
Los contadores de VM incluyen bytes copiados desde RAM, no sólo transferencias
reales del disco.

## Implementación

- `MAP_PRIVATE` usa imágenes por identidad device/inode. La imagen posee
  frames y referencia de mapping; no retiene el descriptor del archivo.
- LRU de 256 slots en equipos de al menos 256 MiB; 32 en equipos menores.
  Presupuesto: 128 MiB con al menos 1 GiB RAM, 64 MiB con al menos 256 MiB,
  8 MiB en el resto. Archivos de hasta 8 MiB; se expulsa sólo una entrada sin
  mappings activos. No se elevó el límite de archivos abiertos: vuelve a 128.
- `read()` y las lecturas ELF completas del runtime reutilizan el cache.
  Las imágenes ELF del runtime se admiten también desde `read_all()` en equipos
  de al menos 256 MiB. El fallback de lectura eager sigue disponible.
- Las vistas compartidas y las escrituras reales al archivo mantienen su
  coherencia y su writeback. Una imagen privada puede promoverse a objeto con
  FD de writeback; al perder la última referencia se libera ese objeto.
  Truncate, rename, unlink y reemplazos nativos invalidan o actualizan el cache.
- Se evita escanear todos los FDs durante búsquedas sin escrituras pendientes.
  Una DLL de sólo lectura no fuerza un flush preventivo para entrar al cache.
- ATA negocia bloques de hasta 16 sectores mediante IDENTIFY/SET MULTIPLE y
  usa READ/WRITE MULTIPLE. Discos sin soporte y canales reseteados usan PIO por
  sector. Se conserva CACHE FLUSH al terminar una escritura; no se eliminan
  barreras de persistencia. ATAPI conserva su transporte anterior.
- Vision cede CPU y no pinta ni ejecuta timers del escritorio durante el splash.
  `[VM:FIXED-FREE]` queda bajo `BLESK_WINE_VERBOSE_TRACE`. Los contadores
  `[WINE:VM-PERF]` y `[WINE:CACHE-PERF]` siguen disponibles; el ioctl existente
  de VM mantiene su ABI, y los contadores nuevos usan un ioctl separado.

No se introdujo demand paging ni COW por página: el COW sigue siendo por VMA.
La siguiente mejora de transporte a estudiar para conservar IDE sería DMA;
AHCI ya permite evitar la parte más costosa del PIO en QEMU.

## Validación y respaldo

- `make -j4 all`: ATA, USB y VMDK construidos.
- BFS4 verifica ATA, USB y el disco de las pruebas, incluidos bitmaps y CRC.
- `private-cache.c`: 100 mappings privados simultáneos, sin aumento de handles;
  aislamiento COW; truncado/invalidation y escritura pendiente; promoción a
  shared; rename y unlink/recreación; expulsión LRU con 300 archivos. Código 0
  en IDE con Pentium III/1536 MiB y con el kernel final en AHCI/512 MiB.
- `shared.c`: offsets y aliases, read/write/msync, protecciones de páginas,
  COW, unlink anterior y posterior al mapping, fork y futex. Código 0 en IDE
  con el kernel final.
- Notepad muestra ventana nativa y recibe `bles` por teclado PS/2. La captura
  durante el warmup sólo muestra el splash; el log libera el escritorio después
  de `[WINE:BOOT] READY`.
- Comparación binaria de la imagen final contra el respaldo: sólo cambian la
  reserva de arranque y los datos/inodes de los dos launchers de Wine. El resto
  del disco conserva sus bytes. USB y VMDK se regeneraron desde esa ATA.

Respaldo: `build/wine-stable-before-fd-neutral/` contiene `ata.img`, el kernel,
el launcher y las fuentes previas. `sources-with-ata.tar.gz` añade el transporte
ATA anterior y las reglas de build a la copia de fuentes. Office no está instalado en esta copia y
no se validó Office 97; el CD-ROM se montó durante las pruebas. Los fallos
históricos de los checks de BSS documentados en la medición anterior no se
consideran resueltos por este cambio.

Logs, hashes y capturas: [wine-fd-neutral-2026-10-07/result.json](wine-fd-neutral-2026-10-07/result.json).
El helper `update-copy.py` de esa carpeta documenta la actualización acotada
usada sobre copias apagadas de esta imagen BFS4: requiere espacio existente
suficiente para los launchers y no es un actualizador general del sistema.

## QEMU con AHCI

Desde la carpeta que contiene `build/`, usando la misma imagen y el mismo CD:

```sh
qemu-system-i386 -accel kvm -cpu pentium3 -smp 2 -m 1536 \
  -drive file=build/bleskernos-ata-user.img,format=raw,if=none,id=osdisk \
  -device ich9-ahci,id=ahci \
  -device ide-hd,drive=osdisk,bus=ahci.0 \
  -vga vmware -display gtk -serial stdio -no-reboot -no-shutdown \
  -drive file=media/installers/OFF97.iso,media=cdrom,readonly=on
```

Para reproducir el gate de Notepad, sobre una copia apagada de la imagen:

```sh
python3 ports/tinyx/scripts/build-stage2-kernel.py /tmp/wine-test/test.img \
  --launcher /SYSTEM/WINE/windows/system32/notepad.exe --association
```
