# W3: transición execve — 14 de septiembre de 2026

El bloqueo de la transición execve quedó corregido. W3 sigue sin aprobar.
Las corridas usan copias desechables del disco y Wine upstream sin cambios.

## Evidencia

- `../build/w3-21-exec-probe/serial.log`: ABI y case30 pasan. El último
  marcador de exec es `step=domains`; wineboot agota el timeout.
- `../build/w3-22-exec-unlock-probe/serial.log`: la sonda adicional llega a
  `domains-done` y `unlock-begin`, pero nunca a `start`.
- `../build/w3-23-exec-publish-fix/result.json`: ABI, case22 (execve con
  PID, argv/envp, CLOEXEC y señales) y case30 pasan. Wineboot sale con estado
  wait 256 (exit code 1), en lugar de quedarse suspendido.
- `../build/w3-24-exec-smp2/result.json`: ABI y case22 pasan también con
  dos CPUs (`passed: true`).

## Causa y corrección

`task_linux_exec_commit()` marcaba la tarea actual como TASK_ZOMBIE y
restauraba las interrupciones antes de activar la tarea reemplazante.
Un cambio de contexto en ese intervalo retira definitivamente la tarea
vieja y deja la nueva en TASK_STARTING.

Ahora `task_start_suspended()` publica la nueva tarea antes de soltar
`g_task_table_lock` y restaurar interrupciones. El lock es recursivo por CPU;
la adquisición interna conserva las interrupciones deshabilitadas.

Los chequeos de layout se ajustaron al linker existente: `.boot` permanece
en 0x00100000 y `.text` residente comienza en 0x00110000. Pasan
`tools/check_high_kernel_layout.py` y `tests/kernel/test_kernel_reserve.py`,
incluidos los rechazos de exceso de imagen y BSS.

## Avance y siguiente bloqueo

W3-23 confirma la ejecución de wine-preloader ET_EXEC sparse (4 segmentos,
6 páginas) y el retorno correcto de la operación que antes pisaba el kernel:

```
mmap2(0x10000, 0x100000, RW, flags=0x32, fd=-1, offset=0) = 65536
```

Más adelante se observa repetidamente:

```
clone3(...) = -38
clone(flags=0x4111, stack=0x60743fe4, ...) = -22
wine: could not exec wineserver
```

El siguiente diagnóstico debe centrarse en el rechazo de ese clone y la
semántica que requiere el arranque de wineserver. No se ha aprobado wineboot
ni la creación completa del prefix. No se comenzó X11 ni se agregaron syscalls.
