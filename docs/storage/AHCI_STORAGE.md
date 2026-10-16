# SATA AHCI en BlesKernOS

El kernel detecta controladores PCI SATA AHCI (clase 01/06, interfaz 01),
configura BAR5 como MMIO sin caché y registra discos ATA en la capa de bloques
como `ahci0`…`ahci3`. Usa DMA con un buffer de rebote contiguo de 64 KiB,
una ranura de comandos por puerto y sondeo. Admite IDENTIFY, READ DMA EXT,
WRITE DMA EXT y FLUSH CACHE EXT. FAT puede montar estos discos; el disco IDE
de arranque existente sigue siendo compatible.

Las escrituras esperan la finalización de FLUSH CACHE EXT. Las áreas MMIO se
reservan frente a mapeos de procesos. Ante un comando fallido el disco queda
inhabilitado para evitar reutilizar buffers mientras pudiera seguir activo
un DMA. La implementación requiere discos ATA con LBA48 y una BAR5 de 32 bits.
No implementa ATAPI, NCQ, interrupciones, hotplug ni recuperación de errores.
El límite actual es cuatro discos SATA.

## Pruebas reproducibles

Con una imagen FAT arrancable, las pruebas montan una copia desechable como
segundo disco AHCI; no modifican la imagen original:

```sh
python3 ports/wine/tests/boot.py --image ports/wine/build/boot4m03/test.img \
  --memory 256 --ahci-image ports/wine/build/boot4m03/test.img \
  --output ports/wine/build/ahci-boot01
python3 ports/wine/scripts/run-runtime.py --image ports/wine/build/full03/test.img \
  --rootfs ports/wine/build/current-rootfs --cases 25 \
  --ahci-image ports/wine/build/boot4m03/test.img \
  --ram-mib 256 --cpus 1 --case-seconds 180 --seconds 300 \
  --expected-text '[AHCI:TEST] DMA read, write, flush and reopen PASS' \
  --output ports/wine/build/ahci-gate01
python3 ports/wine/scripts/run-runtime.py --image ports/wine/build/full03/test.img \
  --rootfs ports/wine/build/current-rootfs --cases 25 \
  --ahci-image ports/wine/build/ahci-gate01/ahci.img --ahci-reuse \
  --ram-mib 256 --cpus 1 --case-seconds 180 --seconds 300 \
  --expected-text '[AHCI:TEST] cold reboot, persisted bytes PASS' \
  --output ports/wine/build/ahci-reboot01
```

Las dos pruebas funcionales pasaron: 128 KiB escritos, leídos y comprobados,
y los mismos bytes verificados tras arrancar de nuevo desde otra copia del
disco. También pasó el arranque con AHCI presente. Son pruebas de corrección,
no una comparación de rendimiento IDE/AHCI ni una validación de `wineboot`.

## Fuentes y licencias

Implementación original bajo la licencia MIT del proyecto. Se usó como
referencia técnica la [especificación pública AHCI 1.3.1 de Intel](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf).
Se consultó la [implementación AHCI de FreeBSD](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/ahci/ahci.c)
para orientar la arquitectura; su licencia es BSD-2-Clause y **no se copió
código**. Por eso este archivo identifica y enlaza esa fuente, sin atribuirle
la autoría del controlador nuevo.
