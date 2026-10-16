# Wineboot: medición del gate W3 (2026-09-27)

La corrida `w3-ahci-profile-01` usó un prefijo nuevo, disco raíz AHCI, un CPU TCG, 768 MiB, runtime completo y perfiles de syscalls/FAT. ABI y caso 25 pasaron; `wineboot-launch` agotó los 2400 s del gate (`-998`). Wine había agotado antes su espera interna del evento de arranque, hacia 1819 s. No hubo fallo fatal del kernel. `kernelbase.dll` apareció correctamente en el prefijo, confirmando la corrección de E/S mayor de 64 KiB.

El volcado de tareas al final registró 95 813 ticks de CPU para `SSLOGO.SCV` (40,9 % de los 234 181 ticks de las tareas listadas). La configuración de la imagen base tenía `enabled=1` y `timeout=300`; el salvapantallas se activó en esta ejecución sin entrada de usuario. Por tanto, el tiempo de la primera corrida no mide solo el trabajo de Wine. Como referencia, `wine-preloader` del instalador consumió 56 405 ticks y `wineserver` 51 677.

El perfil FAT registró al menos 90 escrituras lentas de temporales de registro y 227 de archivos bajo `system32`. Esas son solo las operaciones que superaron el umbral de 10 ticks; no representan el costo total. Varias escrituras reescriben archivos temporales grandes, así que el almacenamiento sigue siendo un objetivo de optimización, aunque este perfil no basta para atribuirle todo el retraso.

El runner ahora desactiva el salvapantallas únicamente en la imagen desechable de un gate Wine. Se inició `w3-ahci-nosaver-01` sin el perfil intensivo, pero se interrumpió a petición del usuario antes de los 300 s de inactividad; el último mensaje de Wine tenía tiempo 146,340 s. No apareció `SSLOGO.SCV` en ese registro. Esta corrida parcial confirma el arranque con la nueva configuración, pero **no cuantifica aún** cuánto acelera Wineboot ni demuestra que termine.

Próximo paso: perfilar los costos de escritura de FAT y reducirlos antes de invertir otra corrida completa. Para comparar el efecto del salvapantallas, una futura medición debe superar los 300 s y registrar el avance de Wine a tiempos invitados iguales, manteniendo igual el resto de la configuración.

## Prueba aislada de FAT, VFS, syscalls y metadata

`storage-layer-01` y `storage-layer-profile-01` ejecutaron el mismo benchmark corto con AHCI, un CPU TCG, 768 MiB y sin Wine. Pasaron ABI, caso 25 y verificación de todos los datos escritos. Los tiempos son del reloj invitado:

| Operación | Sin perfil | Con perfil |
|---|---:|---:|
| 512 `stat` de archivo existente | 1150 ms | 690 ms |
| 512 `stat` de archivo ausente | 630 ms | 3340 ms |
| 256 KiB en 32 escrituras, un `fsync` final | 60 ms | 200 ms |
| Los mismos 256 KiB, `fsync` tras cada escritura | 5430 ms | 2090 ms |
| Escribir 4 MiB y sincronizar | 930 ms | 2460 ms |

La variación entre corridas impide atribuir una latencia fija a cada capa, especialmente a metadata; el perfil y las cachés alteran los resultados. En **ambas** corridas, 32 sincronizaciones costaron mucho más que una: aproximadamente 90 y 10 veces. Esto reproduce el patrón de temporales del registro de Wine. En VFS, `vfs_flush_file()` llama a `fat_write_path()` con el contenido completo de la caché; FAT libera y reasigna la cadena de clusters y vuelve a escribir todo el archivo en cada commit sucio. La prueba verifica que el archivo resultante coincide con los bytes originales.

El perfil de la corrida completa de Wine marcó 3675 syscalls de al menos 10 ticks. Las lecturas (`nr=3`) sumaron 897 964 ticks, pero 1468 de esas lecturas lentas (888 361 ticks) correspondieron a descriptores IPC/dispositivo; son esperas, **no tiempo de lectura FAT**. No se debe usar ese total para justificar cambios al driver de disco. Las escrituras lentas del temporal `reg160000.tmp` sumaron 47 eventos y 807 ticks en el perfil de syscall; el perfil FAT registró 90 commits lentos de temporales de registro, con 4948 ticks en escritura de datos. Los perfiles tienen umbral y las trazas se intercalan, así que son límites parciales.

La optimización prioritaria es un commit incremental y coherente de archivos FAT: evitar reescribir bytes y reasignar clusters que ya están persistidos cuando un `fsync` solo añade unos KiB. Requiere conservar la semántica de handles compartidos, `truncate`, `rename`, `unlink` y persistencia tras reiniciar. Las consultas de metadata son secundarias según estas pruebas; conviene medirlas de nuevo con caché controlada antes de cambiarlas.

## Optimización aplicada y validación

VFS registra el tamaño ya persistido y si las escrituras desde el último commit fueron solo de crecimiento. Tras el primer guardado, `fsync` usa `fat_append_path()`: conserva los clusters existentes, asigna solo los nuevos, escribe únicamente desde el último sector parcial y publica el tamaño al final. Las sobrescrituras y reducciones de tamaño siguen usando la ruta completa. El primer guardado de un archivo vacío conserva la escritura agrupada anterior.

Una primera versión de la ruta incremental escribió cada cluster por separado y empeoró notablemente el rendimiento; se corrigió para agrupar clusters físicamente contiguos antes de aceptar el cambio. En la prueba corta AHCI, las llamadas de escritura a bloque bajaron de 1101 a 883 y los sectores escritos de 18 065 a 10 018. La prueba optimizada incluye además un archivo de bordes que no estaba en la referencia, por lo que el ahorro real del patrón comparable es al menos ese. Los tiempos de reloj variaron mucho por carga/caché del host: `SYNC_EACH` dio 5430 ms antes y entre 2080 y 10 110 ms después; **no se puede afirmar una aceleración de tiempo estable** con estos datos.

La corrida final `storage-append-final-01` pasó ABI, los 30 casos de compatibilidad Linux y el benchmark. Verificó dentro del invitado append a mitad de sector, sobrescritura, truncado y reapertura. Después de apagar QEMU, `mcopy` leyó los cuatro archivos desde la imagen y coincidieron byte por byte. `fsck.fat -n` no informó cadenas perdidas ni cruces; avisó que el resumen de espacio libre de FSInfo estaba desactualizado tanto en la imagen de referencia anterior como en la nueva. Ese contador es un problema previo e independiente de esta optimización.

`storage-append-ide-01` repitió ABI, caso 25 y el benchmark en IDE con código 0. Wineboot completo todavía no se ejecutó con el kernel optimizado; se necesita una corrida separada para medir su efecto real sobre la instalación.
