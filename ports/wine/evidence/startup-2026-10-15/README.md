# Wine + Notepad: arranque y respuesta — 2026-10-15

Esta ronda reduce el coste de cargar Wine y corrige una invalidación que podía perderse en ventanas nativas. La meta solicitada es **20 segundos estables**; un ensayo rápido aislado no basta para darla por cumplida. Los tiempos definitivos y las limitaciones se registran abajo.

## Imagen preparada

`build/bleskernos-wine-responsive.img` contiene el kernel validado sobre una copia de `build/bleskernos-wine-performance.img`. Solo cambian los sectores de Stage 2 y del kernel. Se comprueba que el MBR y **todos los bytes desde el comienzo de BFS4** sean idénticos a la base. No se instalan fixtures, perfiladores ni launchers de prueba en esta imagen. `artifacts.json` registra los hashes.

No se desactivan servicios de Wine, no se precalienta un wineserver en el invitado, no se modifica TinyX y no se cambia la afinidad Linux, el quantum ni la frecuencia del PIT.

## Resultado final: todavía no son 20 segundos estables

El kernel de entrega es `9f7be7cb4ba457b951f58df8eae5d6a2337ae252bfeef9effd83e9e9172e2b96`, archivado en `kernels/release2-kernel/`. Cinco arranques nuevos con ese mismo binario, sin actividad en otra ventana, dieron:

| Ensayo | Wine + Notepad: primera superficie |
|---|---:|
| `yield-start` | 26,403 s |
| `verified-input` | 24,486 s |
| `verified-start1` | 25,403 s |
| `verified-start2` | 21,096 s |
| `verified-start3` | 19,443 s |

**Mediana 24,486 s; rango 19,443–26,403 s. Solo 1/5 queda por debajo de 20 s. La meta de estabilidad sigue pendiente.** `measurements.json` contiene los datos agregados y los hashes de cada ensayo.

En la prueba final con actividad nativa (`verified-native`), Notepad apareció a los **47,356 s**. Se completaron 78 pulsaciones en la ventana nativa antes del cambio de foco: mediana **184,3 ms**, máximo **3272,1 ms**. Todas las muestras completadas detectaron su repintado antes de enviar otra tecla; una superó 2 s. Esto confirma que el escritorio sigue respondiendo durante buena parte de la carga, pero **no demuestra que se hayan eliminado los trabones**. El otro ensayo del mismo kernel, `yield-native`, dio 34,176 s bajo actividad y 70 respuestas completadas, con máximo 993,3 ms; usaba la versión anterior del censor de foco y se conserva separado.

`verified-input` pasó 32 teclas, copiar/pegar y reinicio de Vision conservando la misma ventana y funcionando después del reinicio. Tecla → glifo observado: mediana 125,8 ms, p95 463,7 ms, máximo 485,5 ms. Son medidas de extremo a extremo con el coste de QMP/captura incluido.

No se ocultan las variaciones de los candidatos: el binario anterior `905696...` obtuvo 31,563 / 15,110 / 14,320 / 12,136 / 16,370 s. Aunque 4/5 fueron rápidos, su pico ya impedía afirmar estabilidad. El candidato `0c7dcd...` dio 17,950 / 17,110 / 26,720 / 20,026 s sin actividad adicional y 62,740 s con actividad. Se conservan como resultados exploratorios, no como tiempos de la imagen final. La base de esta ronda (`baseline`) tardó 75,880 s y pasó la prueba funcional; los ensayos de la ronda anterior documentaron 39–41 s. Esta dispersión impide atribuir todo el ahorro a una sola modificación o prometer un tiempo en otro equipo.

## Verificación

- 30 pruebas de host pasan; el inventario con hashes está en `host-tests.json`. Incluyen asignación física, argv/env, E/S por lotes, caché y presión de memoria, permisos/COW/pins, TLB SMP, señales, vida de espacios de direcciones, dominios y repintado nativo.
- BFS4 pasa recuperación, concurrencia, defragmentación, swap-file y **142 cortes entre escrituras + 786 cortes entre sectores** del journal. Se comprueban metadatos y un archivo ajeno tras remontar en cada corte.
- El entorno Linux real pasó ABI y los **33 casos**, incluida la caché privada con 100 imágenes, reutilización de slots y un mapping de 9 MiB con dos páginas físicas limpias. Las aplicaciones nativas y las profundidades de pantalla pasan con **32 MiB**. Swap real con 32 MiB pasó 10737 escrituras y 8143 lecturas de páginas, con cero errores e integridad correcta.
- Esas tres regresiones amplias usaron el candidato `0c7dcd...`; el único cambio posterior de producción fue sustituir la cortesía `task_sleep(1)` por `task_yield()`. El binario final se volvió a probar con Wine real, teclado, portapapeles, reinicio de Vision, entrada nativa y fallo de lanzamiento. El fallo de EXE devuelve 53 y limpia el cursor ocupado.
- La prueba nueva de escritura real genera **4194321 bytes** en fragmentos de 4096, comprueba una segunda descripción antes de fsync, reabre, sobrescribe 3000 bytes cruzando 64 KiB y renombra. Después de salir QEMU se compararon **todos los bytes** desde el host. SHA256 `7f3c7f1fcc24e17e5f43893e7975ea8becf045028970f9ee2ed20cfcc9a1eb42`; modo 0600. BFS4 del disco resultante pasa su comprobación. La evidencia está en `validation/guest-writeback-persistence.json`.
- La imagen de entrega se arrancó desde sus sectores instalados, conservando Vision y los drivers de la base. El escritorio y la prueba de reposo pasan con el hash final `9f7be7cb...`; no se inicia Wine de fondo.
- El primer runner IPC terminó la suite del invitado con PASS, pero rechazó la captura de latencia: la línea de la muestra 0 se mezcló con el informe de RAM por serial. No se usa ese dato como medición completa. Se agregó `--launch-delay` al runner para repetir después del informe de RAM; las pruebas conservan las mismas operaciones. `ipc-repeat` pasa la suite y captura las tres muestras futex y las seis muestras IPC completas. Los tiempos agregados no son latencias individuales ni sustituyen las mediciones de píxeles.

## Cambios conservados

- BFS4 lee hasta 16 bloques contiguos por petición, con EOF y extensiones respetados. La caché sigue limitada a 8 MiB. Con menos de 128 MiB se conserva la lectura original; hay fallback ante OOM o errores de lectura especulativa.
- El asignador de frames continúa desde la última posición libre, en lugar de volver a escanear el bitmap desde cero con las interrupciones desactivadas. Reintenta el pool completo cuando hace falta, incluyendo una reserva que cruza la posición del cursor.
- `exec` copia argv/env por páginas y la E/S de archivos usa lotes acotados de hasta 64 KiB, con fallback por página bajo presión. Se mantienen los límites y los errores de acceso a memoria.
- El loader deja de retener el dominio GUI durante las lecturas del ejecutable. Los diálogos de error conservan su protección local.
- El journal agrupa ocho sectores y lee sus imágenes de deshacer contiguas en una petición. El registro y su contador siguen siendo durables antes de sobrescribir metadatos. Se evita reescribir un superblock idéntico al último checkpoint completado; la recuperación invalida esa instantánea.
- Con RAM suficiente, VFS mantiene hasta 8 MiB de caché de escritura por archivo antes de pasar a streaming. `system.reg` de este prefix tiene unos 1,7 MiB: el antiguo umbral de 1 MiB convertía cada pequeño write posterior en una transacción durable de metadatos. `close` y `fsync` siguen persistiendo datos; ante OOM se vacía el prefijo sucio antes de continuar con streaming.
- El pager fija páginas ya residentes sin volver a adquirir VFS ni publicar los mismos PTEs. Comprueba presencia, privilegio, PFN, permisos y COW antes de incrementar ningún pin. La ruta de carga original conserva los fallos de página y la materialización de COW. No se omiten revocaciones ni sus confirmaciones SMP.
- Una invalidación de contenido nativo asíncrono conserva `content_repaint` hasta programar el siguiente callback. El compositor no puede borrar esa solicitud al terminar un frame anterior. Ventanas síncronas y streaming mantienen su ciclo anterior.
- El compositor anuncia que está esperando sus locks. Una llamada Linux de E/S ya completada puede ceder el turno cuando hay un frame esperando y no quedan dominios retenidos. Esta cortesía no vuelve preemptible todo el kernel.

`production.patch` contiene los cambios respecto a los archivos guardados en `inputs/`. Las pruebas nuevas ejercitan las funciones de producción con backends controlados; no son implementaciones paralelas del algoritmo.

## Método y límites de las mediciones

QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, VMware SVGA, IDE, misma imagen base y mismo prefix. Cada ensayo usa una VM y una copia nuevas; se ejecutan de una en una. La caché del host no se vacía y otras aplicaciones del host siguen ejecutándose. No representa un disco físico frío ni garantiza un tiempo en otro hardware.

`first-surface` cuenta desde el launcher hasta la primera superficie de Notepad, no desde el encendido del OS. El observador mide además el intervalo del host desde que ve el lanzamiento hasta que ve la superficie. La prueba funcional verifica píxeles de teclado/portapapeles antes y después de reiniciar Vision.

La prueba de respuesta durante la carga envía F7 a una ventana nativa y espera un cambio real de su píxel en VRAM. No envía la siguiente tecla hasta recibir la respuesta anterior. La versión final observa el foco real: la creación de Notepad puede transferirlo antes de publicar su primera superficie. Una muestra que cruza ese cambio se censura. Las versiones anteriores censuraban solo al ver el marcador de primera superficie, por lo que una pausa final no es concluyente. Los ensayos exploratorios iniciales reenviaban al vencer 2 s: sus respuestas tardías podían atribuirse a la tecla siguiente y no sirven para afirmar latencias individuales. Las capturas `--load-probe` pausan el invitado y tampoco sirven como tiempos de rendimiento.

## Evidencia de diagnóstico

Los perfiles W3 son builds aislados; sus ámbitos son inclusivos y se solapan entre procesos. No se deben sumar como etapas del arranque. El perfil inicial señaló 808000 llamadas a `pager_pin` y unos 11 s acumulados en ese ámbito; motivó la ruta de páginas residentes. El nuevo readahead redujo drásticamente el número de peticiones de bloques respecto a la ronda anterior.

Las capturas de pausas nativas encontraron Vision y la aplicación dormidos, sin callbacks pendientes y con GUI/GFX/paint libres, mientras Wine seguía haciendo E/S. Esto no prueba que todo el OS estuviera detenido ni que el disco fuera la única causa. La prueba determinista `test_window_invalidation.py` reproduce por separado una solicitud de repintado borrada al finalizar un frame anterior.

## Experimentos descartados y pendientes

Una caché BFS4 de 32 MiB no dio una mejora consistente y se retiró. También se retiró una cesión genérica basada en cualquier waiter: se conservó solo la señal de un frame de Vision realmente esperando. Los fallos iniciales de teclado tras reactivar readahead se siguieron hasta las escrituras pequeñas del registro; las pruebas completas pasan después de corregir VFS. Un arranque rápido sin pruebas de escritura no se consideró suficiente.

Persisten locks gruesos: Vision toma TASK/VFS/GUI/GFX a lo largo de su frame y grandes operaciones CPL0 necesitan puntos explícitos de cesión. Cambiar esa arquitectura exige revisar el orden de locks y la vida de ventanas/recursos; quitar dominios sin esa revisión puede introducir deadlocks o accesos a memoria liberada. La prueba bajo actividad continua debe informarse por separado del arranque en reposo.

## Reproducción

Desde la raíz del proyecto:

```sh
make -j2 build/stage2.bin
python3 tests/kernel/test_vm_frame_allocator.py
python3 tests/kernel/test_exec_strings.py
python3 tests/kernel/test_paged_io_batches.py
python3 tests/kernel/test_pager_ranges.py
python3 tests/kernel/test_paging_tlb.py
python3 tests/kernel/test_vfs_writeback_policy.py
python3 tests/kernel/test_window_invalidation.py
python3 tests/storage/test_readahead.py
python3 tests/storage/test_super_checkpoint.py
python3 tests/storage/test_journal.py --sector-cuts
python3 tests/kernel/run_wine_gui_smoke.py --base build/bleskernos-wine-performance.img --out /tmp/wine-input-check --ram 1536 --smp 2 --vga vmware --wine-debug=-all --latency-samples 32
python3 tests/kernel/run_wine_gui_smoke.py --base build/bleskernos-wine-performance.img --out /tmp/wine-load-check --ram 1536 --smp 2 --vga vmware --wine-debug=-all --load-latency --startup-only
```

Para reproducir un binario archivado se agrega `--kernel-dir` apuntando a su directorio con `kernel.bin`, `kernel.elf` y `stage2.bin`. Los binarios de diagnóstico y de fixtures nunca se escriben en la imagen de entrega.
