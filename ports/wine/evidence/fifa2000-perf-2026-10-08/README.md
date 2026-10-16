# FIFA 2000: rendimiento dentro del partido

KVM, dos CPU, 1536 MiB, AHCI, virtio-vga-gl, SDL/OpenGL, Wine 11.0 y
Gallium VirGL 24.3.4. FIFA usa `dx6` a 640×480. Las capturas muestran el
partido en curso; se excluyen menú, carga y presentación de equipos.

| Configuración | Presentaciones | Intervalo | Presentaciones/s |
| --- | ---: | ---: | ---: |
| CSMT predeterminado | 20 | 31,84 s | 0,63 |
| FIFA con `csmt=0` | 292 | 36,21 s | 8,06 |
| Repetición final con `csmt=0` | 341 | 38,02 s | 8,97 |

Son contadores GLX de una ejecución por variante, en momentos/cámaras distintos
de la misma exhibición. La mejora observada es aproximadamente 13 veces;
no es un benchmark de cuadros idénticos ni una garantía de fluidez. El perfil
todavía no alcanza 30 presentaciones/s. No se certifica un partido completo.
Estas mediciones no incluyen dispositivo de audio. La repetición final conserva
la presentación GLX original, muestra el partido avanzando y no registra errores
de fence del host (`final-csmt0-sdl-host.log` vacío).

`result.json` contiene las muestras inicial/final y las diferencias de tiempos.
Los logs SDL y las capturas permiten revisar el resultado. El perfil probado
se agrega en `HKCU\Software\Wine\AppDefaults\fifa2000.exe\Direct3D` sólo
si no existe un valor `csmt` explícito. El constructor de la distribución y
`make user-update` usan el mismo helper. Se verificaron los hashes de los 63
archivos de aplicaciones instaladas después de actualizar el disco del usuario.

La traza de referencia muestra esperas entre hilos y cesiones de CPU durante
el partido. Actualmente `scheduler_smp_cpu_allowed()` mantiene todas las tareas
Linux en CPU1; evitar el hilo de comandos de Wine reduce esos intercambios.
Wine 11.0 admite el ajuste por aplicación en
[wined3d_main.c](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/wined3d/wined3d_main.c).
No se modificaron las reglas del planificador ni se simularon syscalls faltantes.

Se ensayó también omitir el upload a ventanas ocultas. El probe GLX comprobó
front/back, pixmap oculto, resize y remapeo, pero FIFA no pasó la validación
posterior. Ese cambio fue retirado. `glx-probe-sdl.log` conserva el resultado
del probe; su ejecución posterior sufrió un fallo de fence del host y queda
excluida de las cifras. Un ensayo EGL sin ventana tampoco se usa para comparar.
La versión final conserva la presentación GLX que pasó el partido con `csmt=0`.

Las trazas `[WINE:FRAME-PERF]` y `[WINE:VIRGL-PERF]` requieren
`BLES_WINE_GL_TRACE=1` en el proceso Wine. Sólo el launcher de diagnóstico de
la copia temporal activó esa variable y el autoarranque del juego. Las imágenes
normales conservan el launcher de producción, sin autoarranque ni trazas.
