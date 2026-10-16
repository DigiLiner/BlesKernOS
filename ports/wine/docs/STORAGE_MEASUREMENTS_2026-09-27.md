# Medición de almacenamiento antes de optimizar Wine (2026-09-27)

No se ejecutó una instalación nueva de Wine para estas mediciones. El mismo
kernel, imagen FAT de origen, rootfs, 768 MiB de RAM, una CPU Pentium III y
QEMU i386 se probaron con arranque IDE y SATA AHCI. Cada ejecución utiliza
una copia desechable. El programa `storage-bench.c` mide con
`CLOCK_MONOTONIC` una carga de cálculo, 64 archivos de 1 KiB con `fsync`,
escritura y lectura verificada de un archivo de 4 MiB. Antes del programa,
el kernel lee 4 MiB directamente de bloques en 64 solicitudes de 128 sectores.
El monitor cuenta solicitudes y tiempo acumulado por tipo de controlador.

Comando base (variar `--output`, `--boot-ahci` y `--accel`):

```sh
python3 ports/wine/scripts/run-runtime.py \
  --image ports/wine/build/full03/test.img \
  --rootfs ports/wine/build/current-rootfs \
  --cases 25 --storage-bench --ram-mib 768 --cpus 1 \
  --case-seconds 450 --seconds 600 \
  --output ports/wine/build/storage-ide-02
```

## Corridas TCG completas

Los tiempos son **segundos del reloj invitado**. El orden fue IDE, AHCI,
AHCI, IDE para reducir la confusión con cachés calientes. Todos los datos se
verificaron y los cuatro gates terminaron con código 0.

| Fase | IDE 1 | AHCI 1 | AHCI 2 | IDE 2 |
|---|---:|---:|---:|---:|
| Lectura cruda 4 MiB | 10,44 | 0,69 | 0,04 | 9,10 |
| Cálculo de referencia | 0,05 | 0,16 | 0,03 | 0,08 |
| 64 archivos pequeños + `fsync` | 14,87 | 4,61 | 5,82 | 5,06 |
| FAT: escribir 4 MiB + `fsync` | 21,35 | 2,82 | 3,30 | 23,06 |
| FAT: leer y comprobar 4 MiB | 15,12 | 3,50 | 1,42 | 6,91 |

El orden y la caché influyen especialmente en lecturas y archivos pequeños.
La escritura grande es más estable: IDE 21,35–23,06 s frente a AHCI
2,82–3,30 s, al menos 6,5 veces más rápido en estas cuatro corridas.
La lectura cruda también favorece ampliamente AHCI (IDE 9,10–10,44 s;
AHCI 0,04–0,69 s), pero el extremo de 0,04 s seguramente refleja caché
caliente del host: no representa latencia física de un disco. Los intervalos
de archivos pequeños se superponen; no hay una mejora fija demostrada allí.

En las dos primeras corridas, la fase FAT generó las mismas **620 lecturas
de bloque** (8576 sectores) y **691 escrituras** (8822 sectores) con ambos
controladores. Sumando llamadas por latencia media informada por `perfmon`,
la capa de bloques ocupó aproximadamente 43,2 s con IDE y 8,0 s con AHCI.
La duración de las tres fases FAT fue 51,34 s y 10,93 s, respectivamente;
el resto estimado (8,1 s y 2,9 s) incluye FAT, VFS, syscalls, planificación
y error de medición. **No es una medición aislada de FAT.** La segunda pareja
dio aproximadamente 27,1 s y 8,2 s dentro de bloques, con 35,03 s y
10,54 s de fases FAT. Las 691 escrituras indican muchas operaciones pequeñas;
el controlador AHCI actual vacía la caché tras cada llamada de escritura.

## Referencia de emulación

Una carga de CPU más larga, sobre el mismo disco AHCI y con el mismo
checksum, tardó **3,14 s con TCG** y **0,89 s con KVM** (3,5 veces). Esto
mide el costo de emulación de CPU para ese cálculo, **no** el porcentaje de
CPU empleado por `wineboot`. La fase de archivos pequeños con KVM fue más
lenta que en alguna corrida TCG, de modo que KVM no es un atajo universal
para esta carga de E/S.

Una prueba adicional con IDE+KVM no completó el gate dentro de 600 s de
pared: la escritura FAT de 4 MiB había marcado 410,26 s invitados. Sus
fases parciales se conservan, pero **no se usan** para ratios finales. El
reloj invitado y el tiempo del host difirieron mucho bajo KVM; el tiempo
total de `result.json` incluye además preparación y arranque, así que no
debe confundirse con las fases del benchmark.

## Artefactos y alcance

- TCG completos: `build/storage-ide-02`, `build/storage-ahci-01`,
  `build/storage-ahci-02`, `build/storage-ide-03`.
- Cálculo largo completo: `build/storage-ahci-cpu-tcg-02`,
  `build/storage-ahci-cpu-kvm-01`.
- KVM IDE incompleto: `build/storage-ide-kvm-01`.

En cada directorio están `result.json`, `serial.log`, `command.json`,
`kernel-test.bin` y la imagen desechable. El kernel compiló, su layout
HighMem pasó, y ABI + caso 25 pasaron en los gates completos.

La conclusión práctica es que **IDE PIO era un cuello de botella real**,
especialmente para copias grandes. AHCI lo reduce mucho, pero no elimina
el costo de cientos de escrituras pequeñas y sincronizaciones de FAT. Los
resultados no prueban todavía que `wineboot --init` termine con código 0:
la corrida de Wine posterior al arreglo de lecturas/escrituras grandes se
detuvo a pedido del usuario antes de llegar a QEMU.
