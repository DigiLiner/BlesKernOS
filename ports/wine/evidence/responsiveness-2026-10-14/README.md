# Wine: respuesta y arranque — 2026-10-14

Se corrigió el retiro de tareas dormidas en el scheduler SMP. La corrección mejora IPC y la mediana de escritura de Notepad, pero **no cumple todavía las metas de arranque de 5–10 s ni de escritura de 20–30 ms**. Hay variación entre arranques y picos de respuesta grandes. No se probaron Photoshop ni FIFA 2000: sus instalaciones no estaban disponibles.

## Imagen y cambio conservado

`build/bleskernos-wine-performance.img` es una copia de `build/bleskernos-wine-startup.img` con el kernel corregido. La imagen original permanece intacta. El MBR y todos los bytes del sistema de archivos son idénticos a la base; no lleva fixtures de prueba, trazas adicionales, cambios de TinyX, ni lectura anticipada BFS4. Los hashes están en `artifacts.json`.

En `kernel/sched/task.c`, `task_publish_deferred_ready()` ahora reconoce una espera vencida al terminar el traspaso de pila. Antes, una señal recibida con `running_cpu == TASK_CPU_HANDOFF` adelantaba `wake_tick`, pero el retiro de la pila no publicaba READY: esperaba otro barrido del temporizador. Ese retraso se repetía en intercambios por sockets/futex.

La publicación sigue ocurriendo con la pila antigua ya abandonada, bajo el lock de tareas; la inserción en la cola ocurre después de soltar ese lock. Se mantienen los plazos futuros, la comparación con wrap de ticks, la limpieza de la espera futex y la protección de tareas zombi. No cambian la frecuencia del PIT, afinidad Linux, ABI, ni quantum.

## Comparaciones medidas

QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, VMware SVGA, Wine 11, misma imagen base/prefix y mismo TinyX. Cada ensayo arranca una copia descartable; no hay servidor Wine residente ni calentamiento previo del invitado. La caché del host no se vacía: esto no representa un disco físico frío. `first-surface` empieza al lanzar el programa, no al encender el sistema.

| Ensayo | Disco | Primera superficie | Tecla → píxel p50 | p95 / máximo |
|---|---|---:|---:|---:|
| Base (`baseline`) | IDE | 41,450 s | 293,4 ms | 504,3 ms |
| Base repetida (`baseline-repeat`) | IDE | 39,256 s | 270,0 ms | 297,2 ms |
| Scheduler (`handoff-ui`) | IDE | 30,193 s | 102,8 ms | 1061,5 ms |
| Scheduler (`handoff-ahci`) | AHCI | 27,776 s | 109,0 ms | 784,5 ms |
| Imagen final (`final-ui`, 32 teclas) | IDE | 39,536 s | 93,1 ms | 169,9 / 199,9 ms |

La repetición final confirma la mejora de escritura, pero su arranque de 39,536 s obliga a **no dar por resuelto ni consistentemente reducido el arranque**. También observó el cursor ocupado durante el lanzamiento y su desaparición al abrirse la ventana. Pasaron las 32 teclas, copiar/pegar, reinicio de Vision y escritura/portapapeles después del reinicio, con la misma superficie Wine.

El primer ensayo del scheduler pasó además escritura, copiar/pegar y reinicio de Vision, conservando la misma ventana de Wine y el portapapeles. Los picos muestran que mejorar la mediana no equivale a resolver la respuesta completa.

Otra repetición llegó a primera superficie en 28,496 s (`handoff-repeat`), pero se interrumpió por un error de compilación del observador de colas, corregido después: no cuenta como prueba de teclado. La ejecución `handoff-probe`, con el observador de colas reparado, arrancó en 37,916 s y pasó las 16 teclas con p50 222,2 ms. Su observación añade varias consultas QMP por muestra, por lo que no se mezcla con las mediciones simples. La variación de arranque sigue pendiente de explicación.

Las medidas de teclado son tiempo monotónico del host desde QMP `send-key` hasta que aparece tinta nueva en 16 líneas de VRAM. Pulsaciones de 30 ms, alternando m/w, con pausas de 0,8 s. Se excluye el cursor de texto al contar píxeles. Incluyen coste de captura/QMP (~9–11 ms de mediana por captura) y planificación del host. Los primeros cuatro ensayos usan 16 teclas; el final usa 32. Con 16 muestras, el p95 es también el máximo; no representa un percentil estable de una población grande.

### IPC aislado

Mediana de tres tandas; tiempo del invitado dividido por intercambios, no latencia individual con precisión de microsegundos. El PIT cuantiza el total. Datos completos en `ipc-before` y `ipc-after`.

| Operación | Antes | Scheduler corregido |
|---|---:|---:|
| socketpair, write/read, ida y vuelta | 2473 µs | 60 µs |
| socketpair, poll/read, ida y vuelta | 3493 µs | 40 µs |
| futex, intercambio | 2767 µs | 20 µs |

Las regresiones del invitado pasaron: futex privado/compartido, mappings/COW, señales, timeout, clear_child_tid, 8 hilos y 144000 incrementos; sockets/pipes, SCM_RIGHTS, descriptores >=300, poll/select con varios pares, EOF y transferencia con contrapresión de 300000 bytes. El microbenchmark nuevo `ports/wine/tests/ipc-latency.c` verifica también los bytes transferidos.

## Qué falta en el arranque

El lanzamiento de TinyX y del proceso ocupa menos de un segundo en los ensayos simples. La mayor parte del tiempo transcurre después del spawn y antes de la primera superficie. Los perfiles W3 (`profile`, `profile-after`) cubren una ventana de unos 45 s y contienen ámbitos inclusivos y trabajo concurrente: **sus tiempos no se suman como etapas secuenciales**.

Con el scheduler corregido todavía se observaron ~19959 llamadas de lectura de bloques, unos 65,6 MB y 10,79 s acumulados en ese ámbito. Las escrituras acumularon 4,13 s; ioctl, 5,69 s; clone, 5,50 s; clock_gettime64, 4,90 s. Estos datos priorizan el I/O pequeño y el coste de relojes/servicios, pero no prueban que quitar un ámbito ahorre ese tiempo de pared. No se desactivaron servicios de Wine ni Explorer para obtener un número mejor.

## Experimentos excluidos

- Lectura anticipada BFS4 de hasta 16 bloques contiguos: primera superficie en 23,313 / 22,253 / 20,963 s, pero faltaron teclas en muestras 2 / 3 / 5. El ensayo instrumentado confirmó dos eventos nuevos consumidos por TinyX, sin publicación del glifo. Se retiró del código y de la imagen final. Las pruebas de integridad del almacenamiento sí pasaron: límites, EOF, fragmentación, overwrite/unlink, fallos I/O, OOM, RAM baja, concurrencia, recuperación y cortes de journal. Esto no basta para aprobar la ejecución completa de Wine.
- TinyX con presentación cada 8 ms y sondeo de 5 ms: falló la primera tecla. También se había acelerado el observador de píxeles en ese ensayo; no permite atribuir el fallo únicamente a TinyX. Ninguno de esos cambios se conserva.
- El observador optimizado también se retiró para mantener comparabilidad. El fallo con BFS4 se repitió usando el observador original.

El parche de lectura anticipada queda en `experiments/bfs4-readahead.patch` solo para investigación. Su prueba archivada `.py.txt` debe copiarse a `tests/storage/test_readahead.py` si se reactiva el experimento; no forma parte del conjunto activo. Las trazas de teclado no son mediciones de rendimiento: escribirlas por serial perturba mucho los tiempos. En `tinyx-keytrace2` la primera tecla sí llega al handler de Wine y a WM_CHAR, pero la captura vence a los 5 s; no se usa ese timeout instrumentado como prueba de pérdida de eventos.

## Reproducir

Desde la raíz del proyecto:

```sh
python3 tests/kernel/test_smp_sleep_handoff.py
python3 tests/kernel/test_futex_wake_ready.py
python3 tests/kernel/test_signal_return.py
python3 tests/kernel/run_futex_bench.py --base build/bleskernos-wine-performance.img --out /tmp/bk-ipc-check --program ipc-latency --program futex-latency --program futex --program futex-mapping --program futex-stress --program ipc
python3 tests/kernel/run_wine_gui_smoke.py --base build/bleskernos-wine-performance.img --out /tmp/bk-notepad-check --ram 1536 --vga vmware --wine-debug=-all --latency-samples 16 --startup-probe
```

Los runners usan imágenes descartables. `--kernel-dir` permite comparar kernel.bin/kernel.elf/stage2.bin guardados sin reconstruirlos; `--disk-bus ahci` aísla el transporte. `--input-probe` sirve para localizar etapas y altera la medición. El runner ahora guarda contexto y captura si falta una tecla. Se reparó además el enlace de `--profile-w3` para el comando de link actual.

`handoff-regression-proof.log` demuestra que la nueva regresión de retiro de pila falla con el código anterior y pasa con el corregido. La imagen pasó `tools/bfs4.py check`; conserva la marca dirty ya presente en la base, sin errores estructurales. La prueba `final-failure` también pasó: EXE inexistente termina con código 53 y el cursor ocupado se limpia. Los archivos JSON/log son la evidencia primaria; las metas siguen abiertas.
