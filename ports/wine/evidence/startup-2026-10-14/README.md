# Arranque de Wine y cursor de carga — 2026-10-14

Imagen para probar: `build/bleskernos-wine-startup.img`.
Contiene kernel/Stage 2, Vision, PS/2 y launchers normales sincronizados.
No instala los fixtures ni autoinicios de diagnóstico. La imagen se construyó
sobre una copia de `bleskernos-wine-optimized-v2.img`; las anteriores se conservan.

## Cursor

El contador anterior era estático dentro de `compositor.c`. Kernel y Vision
compilan ese archivo por separado, por lo que Vision no veía el contador del
cargador. Ahora se guarda al final del escritorio compartido, sin desplazar
los campos existentes. El indicador de progreso también prevalece sobre un
cursor personalizado y recupera el cursor anterior cuando termina la carga.

El launcher de Wine activa una referencia mediante `BLES_GFX_LAUNCH_BUSY`
antes de iniciar TinyX. La suelta al registrar la primera ventana o al fallar.
El descriptor es CLOEXEC y su cierre final también libera la referencia: los
hijos ejecutados no retienen el indicador. Las llamadas repetidas son
idempotentes y distintos lanzamientos conservan sus propias referencias.

QEMU verificó el contador activo durante la espera y su retorno a cero, junto
con captura visible del cursor de progreso. Notepad pasó teclado, portapapeles
y reinicio de Vision. La prueba con una ruta inexistente devolvió código 53
y también liberó el cursor. Las pruebas ASan/UBSan cubren varios propietarios,
anidación del cargador, restauración de cursor personalizado y contador sin
underflow.

## Coste de las rutas

`copy_string_from_user` validaba/fijaba/desfijaba la memoria por cada carácter.
Ahora fija una página por vez y recorre sólo hasta NUL. No accede a la página
siguiente antes de necesitarla y conserva el pin del espacio de direcciones.
Se verificaron páginas de guarda, NUL en el último byte accesible, cadenas que
cruzan páginas, truncamiento, fallos de permisos/OOM y referencias equilibradas.

Prueba en Linux i386 real del invitado: tres muestras de 3000 `stat` sobre
Notepad, calentando la primera consulta. QEMU/KVM, CPU host, 2 vCPU, 1536 MiB,
VMware VGA; una VM por vez. Tiempos monotónicos del invitado.

| 3000 consultas | Anterior | Por página |
| --- | ---: | ---: |
| Muestras (ms) | 383,334 / 500 / 466,666 | 260 / 280 / 316,667 |
| Mediana (ms) | 466,666 | 280 |

Reducción del 40% en esta carga, **no en el arranque completo de Wine**.

## Lo que todavía tarda

La referencia de esta sesión dio 39,613 s hasta la primera ventana. Un ensayo
de esperas de varios sockets dio 47,110 s y se descartó; la ruta de sondeo de
producción queda como estaba. Con la copia por página, otra pasada dio
48,756 s (3,836 s hasta TinyX y 43,903 s desde spawn hasta superficie).
**Estas pasadas no demuestran una mejora del tiempo total de apertura.**

La traza de process/service muestra preparación de servicios y Explorer antes
de Notepad, sin un único hueco de 30 segundos que explique toda la demora.
La instrumentación y la carga del host afectan los valores. No se presentan
los tiempos de la traza verbosa como referencia de rendimiento. Queda por
perfilar el coste de crear/inicializar los procesos de Wine y sus intercambios.
No se quitaron servicios ni se cambió la afinidad a una CPU. FIFA/Photoshop no
están disponibles y no se probaron.

El ensayo IPC descartado pasó plazos, señales, EOF, múltiples pares,
descriptores altos, SCM_RIGHTS y backpressure, pero no produjo la mejora buscada.
Su log se conserva para explicar el descarte; no está en la imagen.

## Reproducir

```sh
python3 tests/desktop/test_launch_cursor.py
python3 tests/kernel/test_usercopy_string.py
python3 tests/kernel/test_usercopy_lifetime.py
python3 tests/kernel/run_futex_bench.py --out /tmp/path-prueba \
  --base build/bleskernos-wine-startup.img --program path-lookup
python3 tests/kernel/run_wine_gui_smoke.py --out /tmp/wine-cursor-prueba \
  --base build/bleskernos-wine-startup.img --ram 1536 --vga vmware \
  --wine-debug=-all --startup-probe --latency-samples 8
python3 tests/kernel/run_wine_gui_smoke.py --out /tmp/wine-error-prueba \
  --base build/bleskernos-wine-startup.img --ram 1536 --vga vmware \
  --wine-debug=-all --startup-probe --launch-failure
```

Usar directorios nuevos. Los runners trabajan sobre copias. `inputs` conserva
los archivos anteriores; `production.patch`, los cambios de esta ronda.
`artifacts.json` identifica los binarios, fuentes e imagen final; BFS4 y el
contenido de los payloads instalados se comprobaron.
