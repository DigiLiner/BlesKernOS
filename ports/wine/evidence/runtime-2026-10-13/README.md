# Diagnóstico inicial de respuesta de Wine — 2026-10-13

Actualización posterior: las pruebas sincronizadas de kernel/Vision sí abrieron
Notepad. El ensayo de 150 s de este documento no es una referencia válida para
atribuir mejoras de arranque ni demostrar un deadlock. Ver la
[optimización y validación posterior](../optimization-2026-10-13/README.md).

Pedido: medir y revisar causas antes de optimizar. No se modificaron las
implementaciones de producción de Wine, drivers, scheduler ni las imágenes
habituales. Las modificaciones persistentes son herramientas de diagnóstico
y esta evidencia. Los guests sí escriben en sus propias copias temporales.

## Entorno y alcance

QEMU/KVM, CPU Pentium III, 2 vCPU, 1536 MiB, VGA VMware, display sin ventana.
Discos temporales derivados de `build/bleskernos-ata-user.img` del 11 de octubre,
con un kernel de diagnóstico enlazado desde los objetos actuales. No equivale
a probar VMware Workstation, hardware real o la sesión donde se informó el fallo.

El kernel actual difiere de las mediciones del 7 de octubre: registra
`[WINE:BOOT] deferred until Windows program launch`. No corresponde comparar
este arranque en frío con la apertura posterior a aquel warmup.

## Mediciones de esta sesión

| Ensayo | Resultado | Interpretación |
| --- | --- | --- |
| IDE, launcher normal, estados de tareas cada 10 s | No se obtuvo ventana; aviso TLB y watchdog CPU1 sin progreso durante 5 s | Diagnóstico, no benchmark de latencia |
| AHCI, launcher con trazas y perfil periódico | Proceso creado a 2090 ms guest desde el launcher; no apareció ventana durante la observación de 150 s host | Hubo fallo AHCI `cmd=0xea` y watchdog; excluir de comparaciones de rendimiento |
| IDE, trazas de launcher, sin perfil periódico ni volcado de tareas | Proceso creado a 2310 ms guest; no apareció ventana durante 150,29 s host desde QEMU | El aviso TLB también ocurre sin el perfil periódico |

En el último ensayo IDE, el gate empieza a los 13,451 s host y el mensaje de
spawn se recibe a los 16,040 s host. Quedan unos 134 s de observación después
del spawn sin ventana. El prefijo ya tiene el marcador de preparación:
`PREFIX ready marker=1`. El mensaje de spawn sólo acredita creación del
proceso; **no mide programa listo ni primera imagen**.

Las capturas periódicas buscan más de 20.000 píxeles blancos como indicio de
Notepad. La captura final muestra sólo el escritorio y tampoco hay un mensaje
`first-surface` del launcher. El detector de blancos no sirve como prueba
general de disponibilidad para otras aplicaciones.

Los JSON iniciales pueden contener líneas seriales partidas entre lecturas;
para los valores guest se utiliza el `.log` completo. El observador ya conserva
los fragmentos hasta recibir el salto de línea. Los tiempos host de recepción
incluyen retraso del sondeo, la salida serial y las capturas.

Una repetición IDE sobre el disco temporal ya usado, sin perfil periódico,
leyó directamente el contador PIT por QMP. En el tramo posterior a los primeros
35 s: **28,19 s guest frente a 28,258 s host**, cociente 0,9976, 43 muestras.
No hay evidencia de un reloj permanentemente ralentizado en ese tramo. Las
consultas tardaron hasta 224 ms: esta medición permite comparar el avance
acumulado, no certificar precisión de milisegundos ni excluir pausas cortas.
La observación completa duró 67,81 s y tampoco produjo ventana. El arranque
inicial tiene más desfase; no se usa como prueba de un reloj estable.

No se obtuvo una medición válida de tecla a píxel, ni una ejecución nueva de
Photoshop o FIFA. Los avisos del kernel justifican investigar esperas, pero
no demuestran por sí solos la causa de la torpeza descrita por el usuario.

## Puntos concretos encontrados en el código

1. **Concurrencia limitada.** `kernel/sched/scheduler_smp.c:53` obliga a todas
   las tareas Linux a CPU1 cuando hay al menos dos CPU. Wine, wineserver,
   TinyX y los hilos del programa comparten ese núcleo. La restricción está
   documentada como protección de VM/señales; no debe eliminarse sin resolver
   esas condiciones y validar SMP.
2. **Sondeo de entrada.** `ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/src/kinput.c:1459`
   llama a `pollEvents` y fija un timeout de 20 ms. El launcher también usa
   `usleep(20000)` para eventos de superficies. Son esperas en rutas distintas;
   no se suman automáticamente como una latencia medida de teclado.
3. **Publicación limitada y trabajo aun en reposo.** `kdrive/fbdev/blesos.c:90`
   comprueba un intervalo de 33 ms y calcula dos hashes recorriendo todos los
   píxeles. Si cambian, `kernel/linux/i386_ipc.inc:702` copia el framebuffer y
   las regiones de ventanas antes de publicar a Vision. Esto puede costar CPU
   y limitar la fluidez, aunque la aplicación procese rápido sus eventos.
4. **Relojes y esperas.** `kernel/core/compat_mode.c` selecciona PIT de 100 Hz
   y GUI de 30 FPS para Pentium III. `SYS_LINUX_NANOSLEEP` redondea hacia arriba
   al tick: una espera positiva inferior a 10 ms solicita un tick en este
   perfil. Los relojes Linux monotónicos también se construyen con ticks PIT.
   Esto puede afectar animación y temporización; falta un A/B que mida su peso.
5. **3D con tránsito por CPU.** `ports/wine/graphics/glx.c:239` resuelve/readback,
   crea una XImage y hace XPutImage. Después siguen TinyX y la composición
   nativa. Una GPU acelerada no elimina ese recorrido.

## FIFA: evidencia anterior, no repetida hoy

La [ronda del 8 de octubre](../fifa2000-perf2-2026-10-08/README.md) conserva
436 presentaciones en 44,39 s host: **9,82/s**, sin audio. El perfil de aquella
ronda señaló el reescalado bilinear de TinyX (`fbFetchTransformed`).
El benchmark GLX sencillo llegó a 58,4 presentaciones/s, que no equivalen a
FPS visibles ni a una medición aislada de GPU. Hay indicios de costes de
Wine/XRender/copias y contención, no una demostración de que toda la pérdida
sea rasterizado. El backend VMware clásico de esa evidencia exponía OpenGL
1.2 sin GLSL; no se puede equiparar con VirGL.

## Orden propuesto para la siguiente etapa

Primero conseguir un arranque reproducible de Notepad con la configuración
real del usuario y separar los fallos TLB/almacenamiento de las mediciones.
Después medir tecla a píxel y frame times p50/p95/máximo en reposo, escritura
y menús. Para FIFA, repetir la misma escena de 60 s, resolución y audio, tres
veces, registrando backend y reloj host. Sólo entonces hacer A/B de sondeo,
publicación, reescalado y copias. No quitar la restricción SMP como atajo.

## Herramientas y validación

Se reparó `ports/tinyx/scripts/build-stage2-kernel.py`: import de un helper
existente, resolución de includes tras mover el kernel y header del loader.
El gate ahora duerme mientras espera para no competir continuamente con la
aplicación. `--runtime-perf` es opcional; el perfil publica cada 10 s. Los
volcados completos de tareas usados en las primeras exploraciones se quitaron
de esa opción por su coste e interferencia.

`observe.py` usa reloj monotónico host, conserva serial y capturas, y lee
`kernel_ticks` mediante QMP usando los símbolos del ELF exacto. `clock_probe.py`
permite esa lectura sola: no conectarlo simultáneamente al mismo endpoint QMP
usado por el observador. Las direcciones sólo valen para el kernel medido.

Validación: compilación y enlace de los kernels aislados, arranque real de
QEMU, y `py_compile` de los tres scripts. No se actualizó ninguna imagen de
uso habitual ni se presenta una optimización como validada.
