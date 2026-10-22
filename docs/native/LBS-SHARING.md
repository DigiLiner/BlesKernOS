# LBS: páginas físicas compartidas entre procesos

Implementado el 2026-10-18. Los clientes migrados y recompilados usan mappings
respaldados por archivo para los segmentos de código y constantes de las LBS
nativas PIC. Las bibliotecas siguen directamente en `/SYSTEM`.

## Mecanismo

El cargador reserva el espacio virtual de cada imagen usando las syscalls VM
existentes (55–57). Superpone los segmentos PT_LOAD sin PF_W mediante mappings
`MAP_PRIVATE` del archivo original abierto, con permisos RO/RX. El pager existente
reutiliza los marcos limpios asociados al mismo archivo. No se copian esos
segmentos a un heap por proceso. Se comprueba su contenido contra el archivo
validado, por lo que esta entrega toca inicialmente las páginas de sólo lectura.
No se presenta como una optimización de arranque bajo demanda.

Los segmentos escribibles, datos inicializados, BSS, GOT y destinos de relocación
son privados. Las relocaciones de las LBS con VM sólo pueden escribir dentro de
PT_LOAD escribibles. Se rechazan text relocations, segmentos RWX, páginas PT_LOAD
superpuestas y alineaciones incompatibles. Las LBS producidas por el build
cumplen estas condiciones. El camino `.SO` heredado mantiene su comportamiento,
incluidas sus relocaciones de texto; sus imágenes no usan esta nueva modalidad.

Una página de archivo marcada explícitamente escribible mediante `mprotect`
sigue siendo privada: el primer write dispara COW. Sin esa autorización, un
write en código RO termina el proceso. No existe una nueva syscall ni cambio de
ABI pública. El backend LBS llama directamente al VM nativo; no cambia el shim
POSIX heredado de `mmap`. Los ejecutables con el cargador anterior incorporado
necesitan volver a enlazarse para compartir físicamente sus LBS.

## Ajustes del kernel necesarios

El fallo de página COW ahora consulta el permiso COW real de la página, también
para procesos nativos. Antes ese camino estaba restringido a la personalidad
Linux. Las escrituras sobre páginas realmente RO siguen fallando.

Las puertas de retorno de aplicación y upcall pasan a una página dedicada,
alineada y de sólo lectura accesible desde usuario. Antes ocupaban la primera
página del heap del kernel, que los nuevos espacios privados mantienen protegida.
Se conserva la protección supervisor de esa página de metadatos del allocator.

La imagen instalable incluye kernel y stage2 actuales: stage2 lleva el checksum
del kernel y ambos deben coincidir. `tools/lbs_kernel.py` valida el par antes de
instalarlo. La primera etapa, configuración, datos y prefijo Wine se conservan;
la imagen original no se modifica.

## Evidencia física

`tests/lbs/run_sharing.py` arranca dos procesos y observa las tablas reales a
través de QMP. No añade una interfaz de direcciones físicas al sistema invitado.
Los offsets de estructuras se obtienen del fuente del kernel compilado.

En QEMU/KVM con **64 y 256 MiB**, y también en QEMU/TCG con **64 MiB**, los
procesos tienen CR3 distintos y comparten
**373 páginas RO de 4096 bytes: 1.527.808 bytes, unos 1,46 MiB que no se duplican**
en esa escena (NGS3D y sus dependencias más una biblioteca fixture). Cada página
observada tiene el mismo marco físico en ambos procesos y carece de permiso de
escritura. Ésta es una medida del código y constantes compartidos en esa prueba,
no del ahorro neto de toda la RAM del sistema.

La prueba también verifica:

- Marcos físicos distintos para datos privados, con valores independientes.
- Una copia física nueva al escribir tras `mprotect`; el otro proceso conserva
  la constante original y continúa ejecutando después de la primera descarga.
- Rechazo de escritura por `bk_read` sobre código, y muerte del hijo que intenta
  modificar código RO directamente (un fallo de página intencional).
- Liberación de los espacios y retorno del caché de archivos a su estado inicial
  después de la última descarga.
- Puertas de retorno RO en ambos espacios, con metadatos del allocator supervisor.

Resultados detallados, direcciones y marcos por página:
[64 MiB](../../tests/lbs/evidence/sharing/kvm-64m/result.json) y
[256 MiB](../../tests/lbs/evidence/sharing/kvm-256m/result.json), más
[TCG 64 MiB](../../tests/lbs/evidence/sharing/tcg-64m/result.json).

La integración completa pasó 274 comprobaciones con VGA estándar y 276 con
VirGL. Una primera ejecución VirGL agotó su espera en la ventana Vision, sin
fallo de página; la repetición completó la prueba. No se atribuye una causa que
no se haya demostrado. También pasaron los 25 clientes de escritorio con 64 y
256 MiB, la regresión funcional de edición/preferencias y el launcher Wine con
Notepad. Los resultados están en `tests/lbs/evidence/`; los de esta actualización
usan `sharing/` y el sufijo `-shared` en las pruebas de migración.

El cargador i386 en host comprueba además rollback ante 12 fallos VM y 25 fallos
de asignación, 400 ciclos concurrentes con mappings reales, rechazo de text
relocations/RWX/solapamiento y compatibilidad de text relocations `.SO`.

## Contabilidad y límites

`bk_dl_statistics.image_bytes` conserva su ABI y mide tamaño virtual. La nueva
`bk_dl_mapping_statistics` separa bytes virtuales de archivo RO y privados; no
cuenta marcos únicos globales. El contador de memoria del proceso que refleja
heap tampoco incluye toda su memoria VM: su descenso no demuestra ahorro físico.

El bootstrap estático, heap, datos y tablas de páginas consumen memoria propia.
El ahorro neto depende del número de procesos y bibliotecas comunes y del coste
de sus mappings. Los binarios pequeños pueden crecer por el bootstrap, aunque
los CPL que eliminan Mesa estática se reducen considerablemente.

i386 sigue sin NX; no se promete impedir ejecución de datos ni RELRO completo.
El caché se libera cuando termina la última referencia. No se ha añadido una
política de inmutabilidad de archivos: actualizar una LBS en uso puede afectar
mappings según la semántica existente del VFS. La instalación se hace offline
y los cambios se aplican al reiniciar; no sobrescribir bibliotecas en uso.
No se ha probado en hardware físico. Wine conserva su backend Linux/Mesa/VirGL;
el launcher nativo usa SYSTEM.LBS, sin forzar ABI nativa dentro del Wine Linux.

## Reproducción e instalación

```sh
make -j4 lbs-audit
python3 tests/lbs/run_sharing.py --memory 256 --accel kvm
python3 tests/lbs/run_sharing.py --memory 64 --accel kvm
python3 tools/install_lbs.py
python3 tests/lbs/verify_installation.py
```

Salida: `build/bleskernos-lbs-user.img`. El instalador rechaza el mismo archivo
como origen y destino, y discos abiertos por QEMU. La documentación instalada
incluye `/SYSTEM/DOCS/LBSSHARE.TXT`.
