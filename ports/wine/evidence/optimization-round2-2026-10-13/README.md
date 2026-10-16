# Segunda ronda: despertar de futex — 2026-10-13

Se elimina una espera al siguiente tick del PIT en `task_linux_futex_wake`.
Un hilo que ya dejó su pila pasa a READY y se encola después de soltar el
spinlock. Si todavía ejecuta WAIT o está entregando su pila al scheduler,
conserva la ruta protegida anterior. No se cambia la afinidad de Wine ni
la frecuencia de los timers, de TinyX o del compositor.

## Prueba directa en el sistema invitado

QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, VMware VGA, una VM por vez.
Dos hilos intercambian turno con FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE;
20 vueltas de calentamiento y tres muestras de 1000 idas y vueltas.
El contador de esperas confirma que los hilos realmente se bloquearon.

| Medida | Antes | Después |
| --- | ---: | ---: |
| Mediana por ida y vuelta | 3,503 ms | 2,283 ms |
| Tres muestras por ida y vuelta | 3,503 / 3,333 / 4,263 ms | 2,283 / 2,317 / 2,277 ms |
| Contención: 8 hilos, 1500 rondas, 144.000 incrementos | 22,683 s | 13,067 s |

Son reducciones del 34,8% y 42,4%, respectivamente, en estas cargas de
sincronización. Los tiempos de ida y vuelta usan el reloj monotónico del
invitado; el tiempo de contención usa la recepción de marcadores seriales
en el host. No equivalen a mejoras de FPS o de tiempo total de aplicaciones.

## Correctitud y escritura

Las dos variantes pasaron el suite real de futex: WAKE, límites, timeout,
EINTR por señal, pthread_join/clear_child_tid, aislamiento de mapeos privados,
COW y estrés con mutexes, barreras y variables de condición.

La variante nueva pasó además Notepad, copiar/pegar y escritura antes y
después de reiniciar Vision. La medición de teclado incluye el coste y la
perturbación de consultar el framebuffer mediante QMP. La primera pareja
mostró 359 → 281 ms de mediana, pero una segunda referencia sin el cambio
volvió a 278 ms: **no se atribuye una mejora estable de escritura a futex**.
La segunda pareja completa dio 277,80 → 348,04 ms, con mayor coste de
captura en la variante nueva. La dirección cambió entre pasadas: los datos
no permiten aislar mejora ni descartar una regresión de escritura.
El texto todavía tarda demasiado en aparecer. FIFA y Photoshop no están
instalados en las imágenes disponibles y no fueron medidos.

`test_futex_wake_ready.py` ejecuta el código real con ASan/UBSan y comprueba
máscaras, alcance, cantidad máxima, repeticiones, UP y la propiedad de las
pilas activas/en transición. `test_smp_sleep_handoff.py` y
`test_signal_return.py` cubren la entrega de pila y el retorno de señales.

## Reproducir

```sh
python3 tests/kernel/test_futex_wake_ready.py
python3 tests/kernel/test_smp_sleep_handoff.py
python3 tests/kernel/test_signal_return.py
python3 tests/kernel/run_futex_bench.py --out /tmp/nuevo-futex-test \
  --base build/bleskernos-wine-optimized.img
python3 tests/kernel/run_wine_gui_smoke.py \
  --base build/bleskernos-wine-optimized-v2.img --out /tmp/nuevo-wine-ui-test \
  --ram 1536 --vga vmware --wine-debug=-all --latency-samples 16
```

Los runners usan copias desechables. Para comparar la versión anterior,
`inputs/i386_futex.inc` conserva el archivo previo y `production.patch`
contiene el único cambio de producción de esta ronda. Restaurar el archivo
optimizado después de reproducir una referencia. Los JSON, logs, capturas y
hashes permiten revisar los resultados sin volver a ejecutar las VMs.

## Imagen

`build/bleskernos-wine-optimized-v2.img` contiene el nuevo kernel y Stage 2
sobre la imagen optimizada de la ronda anterior. Se verificaron sus bytes;
el resto de la imagen, incluidos el prefijo y los datos de usuario, es
idéntico a la anterior. BFS4 pasó su comprobación (el indicador dirty ya
estaba en la imagen anterior). Se conservan tanto la imagen base habitual
como `build/bleskernos-wine-optimized.img` para comparación.

Para probar la nueva ronda, arrancar **optimized-v2.img**. No basta con
arrancar la imagen habitual: ésta permanece sin modificaciones.

Los hashes de las imágenes base al cerrar esta ronda están en `artifacts.json`.
Las imágenes disponibles al comenzar ya eran posteriores a los hashes del
informe de la primera ronda; no se comparan esos hashes históricos como si
fueran la misma generación. Todas las pruebas A/B de esta ronda usan la misma
imagen `optimized.img` disponible en esta sesión.

## Cierre de la prueba UP pendiente

El intento con una sola CPU pasó latencia, timeout/WAKE/EINTR y mapeos
privados, pero el estrés de ocho hilos no terminó dentro de 300 segundos.
El runner detuvo QEMU. No se considera PASS del suite UP ni se atribuye
la causa a futex sin una comparación UP con el kernel anterior.
