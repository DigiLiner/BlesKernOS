# Balance y punto de parada

Trabajo detenido a pedido del usuario. No continuar compilando ni probando
hasta retomar explícitamente la optimización. Las VMs y compilaciones están
cerradas. Esta ronda no actualizó `build/bleskernos-ata-user.img`; los hashes
de los 63 archivos instalados comprobados siguen intactos.

## Qué cambió y qué demuestra

| Cambio | Evidencia | Estado al detener |
| --- | --- | --- |
| `csmt=0` sólo para FIFA | Ronda anterior: 0,63 → 8–9 presentaciones/s en partido | Ganancia grande ya obtenida; respetar preferencias explícitas |
| Copias parciales de buffers VirGL | Tests de rangos y VBO real con guardas | Correcto en pruebas; evita copiar la asignación completa |
| Swap con un único flush y GC X11 reutilizado | Probes reales front/back, pixmap oculto, resize/remap | Validado en QEMU |
| Submit asíncrono y staging privado | Tests con host retrasado, readback, fences públicas, shader/FBO/VBO reales | Validado en probes y una corrida de partido; estabilidad prolongada pendiente |
| Staging de vértices SVGA3D con fences por bloque | Test de 16 vueltas y selftests VMware reales | Correcto; ganancia pequeña y variable en benchmark |
| Bilinear rápido TinyX | Comparación exacta de millones de píxeles y menú visible | Experimental, apagado por defecto; paquete restaurado al TinyX anterior |
| Desktop virtual 640×480 | Imagen sin ampliar, falló teclado | Rechazado como default |

Los cambios GPU y sus dependencias están en las fuentes de las builds normales
incluido el actualizador de VMWARESVGA3D.DVR. Eso no equivale a haber desplegado
esta ronda en el disco habitual. `build/bleskernos-normal-check.img` es una
imagen de prueba construida antes de restaurar TinyX; contiene el experimento,
no debe confundirse con una imagen final validada.

## Rendimiento observado

- Partido, sin audio, TinyX anterior y nuevos cambios GPU: 436 presentaciones
  en 44,39 segundos host, **9,82/s**. La referencia anterior era 8–9/s.
  Las cámaras y momentos difieren: no permite asignar un porcentaje fiable
  de mejora a estos cambios. Sigue sin ser fluido.
- Benchmark GLX/X11 simple a 640×480: aproximadamente 24,3 → 58,4/s.
  Con 1024 lotes por cuadro: 23,5 → 24,4/s. Es una escena sintética corta,
  no FPS de FIFA ni refresco visible. La mejora depende de la carga.
- VMware 640×480: 91,8 → 108,3/s con un lote, 90,4 → 87,1 con 128 y
  77,8 → 79,8 con 1024. No hay ganancia general demostrada.

El rendimiento del partido no parece un techo fijo del rasterizador emulado:
la ruta X11 presenta escenas simples más rápido. Sin embargo, no medimos un
límite exclusivo de la GPU. El perfil señaló Wine y el bilinear de TinyX;
las tareas Linux comparten CPU1, hay readback, copias y composición por CPU.
TinyX publica a la GUI cada 33 ms, alrededor de 30 Hz, aunque GLX/X11 pueda
recibir más cuadros.

## Qué impide seguir sacando conclusiones

Tres pruebas posteriores registraron `Failed to create fence sync object`
en el host, timeout de lectura en el guest y, en una, cierre de FIFA. Se
excluyen de rendimiento. También ocurrió sin audio. La repetición con TinyX
anterior llegó al menú sin error en el intervalo observado y se detuvo antes
de otro partido: todavía no aísla la causa. La instrumentación temporal del
host no pertenece al producto. No se certifica una corrección del fallo.

El intento con AC97/PulseAudio tuvo errores de volumen/mute y el fallo de
fence; **no hay una medición válida del partido con sonido**.

En VMware, el backend clásico actual expone OpenGL 1.2/Mesa 3.5 sin GLSL.
Wine 11 no pudo iniciar FIFA acelerado. La mejora del FIFO/staging no agrega
shaders: falta un backend Gallium SVGA programable. No mezclar ese trabajo
con los ajustes de rendimiento del backend VirGL que ya ejecuta el juego.

## Cómo retomar

1. Congelar una referencia reproducible: mismos binarios, cámara, resolución,
   configuración y recorrido dentro del partido; medir por reloj host durante
   al menos 60 segundos, con calentamiento y tres repeticiones. Guardar hashes,
   log guest, log host y una captura antes/después. Separar sonido apagado y
   encendido.
2. Resolver primero el fallo de fence. Repetir esa referencia con TinyX anterior
   y con submit síncrono/asíncrono, cambiando sólo una variable. Un fallo del
   host invalida la corrida; no atribuirlo al último cambio sin un A/B.
3. Perfilar el partido estable por etapas: juego/Wine, traducción Direct3D,
   ioctls/esperas GPU, readback, XRender, publicación TinyX y compositor. Distinguir
   tiempo ejecutando de tiempo esperando, y FPS producidos de cuadros visibles.
4. Elegir únicamente el mayor costo medido. Probar la ruta bilinear experimental
   sólo si sigue siendo dominante; conservar bordes, alpha, entrada y calidad.
   No abrir el SMP Linux sin auditar TLS y supuestos de concurrencia.
5. Para VMware, tratar Gallium SVGA como proyecto de compatibilidad aparte:
   winsys/recursos, capacidades y shaders, luego probes de color/profundidad/
   sincronización, y recién después FIFA. Antes de eso no existe un benchmark
   acelerado del partido que permita optimizarlo.

Criterio para aceptar cambios: mejora repetible en el partido, sin cierres,
artefactos ni regresiones de entrada/audio; integrarlos de uno en uno. Si
la diferencia queda dentro de la variación entre corridas, no acumularlos.
Los datos completos y las nueve pruebas locales PASS están en `result.json`.
