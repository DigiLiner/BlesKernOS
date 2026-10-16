# Escritorio, archivos y audio de Wine

La miniatura de Pantalla mantiene las proporciones del fondo y recorta el dibujo
al vidrio del monitor. Las pestañas activas sobresalen y se unen al panel; ventanas
y controles tienen sombras más marcadas. El administrador de archivos separa los
botones y reproduce el sonido de botón al abrir carpetas, si está habilitado.

Los archivos del escritorio se guardan en `/SYSTEM/DESKTOP`. Arrastrar un icono
cambia su posición, que se conserva al reiniciar. Arrastrar archivos hacia una
carpeta o ventana del administrador permite transferirlos. En el mismo volumen
se mueven; entre volúmenes se copian, conservando el original, incluso desde CD.
Una copia se publica después de completarse y nunca reemplaza otro archivo.
El movimiento comprueba las rutas en el VFS para impedir copiar una carpeta
hacia sí misma mediante un alias del volumen.

Eliminar, el botón de la barra, el menú contextual y arrastrar a Papelera envían
los archivos a `/SYSTEM/TRASH`. Dentro de Papelera, Restaurar devuelve un elemento
a su ruta original si está libre; Eliminar confirma su borrado definitivo.
Los iconos incorporados del escritorio se pueden ocultar sin borrar su programa.
La seleccion multiple permite mover o enviar varios elementos a Papelera en una sola accion.
La papelera requiere poder renombrar en el volumen del sistema. Un CD o un archivo
de otro volumen no se borra silenciosamente si no puede enviarse a ella.

GAME/GAMES y RadioScope se retiraron; Tux Racer conserva su código y se construye
con `make extras`, pero no se incluye en la imagen estándar.

Wine usa `wineoss.drv` y los dispositivos Linux emulados `/dev/dsp` y `/dev/mixer`.
El puente convierte PCM U8/S16LE mono/estéreo de 8–48 kHz a la salida nativa
U8 mono de 44,1 kHz y mezcla colas acotadas. No implementa captura ni MIDI.
QEMU debe presentar una tarjeta soportada por nuestros controladores. El runner
`tools/run_qemu_virtio.py` agrega AC97 con PulseAudio/PipeWire por defecto:

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-ata-user.img --gl
```

Si se usa un comando propio, agregar `-audiodev pa,id=blesaudio
-device AC97,audiodev=blesaudio`. `--audio alsa`, `--audio sdl` y `--audio none`
permiten elegir otro backend del runner.

La actualización incremental `make user-update`, con QEMU cerrado, sustituye
kernel, escritorio, controles, administrador y módulos de audio mediante una
imagen temporal validada; conserva el prefijo de Wine y los archivos instalados.
No reconstruir el disco para aplicar estos cambios a una instalación existente.

Chequeos automáticos: `tests/desktop/test_visuals.py`, `test_file_ops.py` y
`test_audio_bridge.py` y `test_transfer_paths.py`. La prueba Windows `ports/wine/tests/audio-compat.c` abre
waveOut, reproduce medio segundo y espera la finalización. La evaluación visual
con ratón y el sonido audible de aplicaciones reales se comprueban en la VM.

Resultado de la prueba en QEMU/KVM, dos CPU, Virtio 2D y AC97 con backend WAV:
Wine cargó wineoss.drv, enumeró una salida y completó waveOut. La captura contiene
PCM adicional al sonido de inicio. Con el diagnóstico de audio activo se observaron
interrupciones entre fragmentos; esta primera salida usa el API nativo de clips,
no un flujo DMA permanente. La continuidad bajo carga y el estéreo requieren
trabajo adicional; no se consideran validados en juegos o aplicaciones reales.

El stub WINEOSS.DRV se instala también en System32 mediante `make user-update`.

## Opciones y selección del escritorio

Click derecho → Escritorio abre un submenú al pasar el cursor. Incluye organización
automática, alineación a cuadrícula, iconos pequeños (32), medianos (48) y grandes
(64 píxeles), y Organizar ahora. Las preferencias se guardan en `.bk-layout`.
La organización automática está activada inicialmente; desactivarla permite
colocar iconos libremente. Sus posiciones se conservan al reiniciar.

La papelera usa el recurso clásico `RecycleEmpty` de GRAPHICS.PAK. Al arrastrar
se dibuja una silueta con el contorno y una trama gris sobre las partes opacas del
icono, respetando las zonas transparentes. Los iconos originales conservan su
posición hasta soltar el botón. Un grupo tiene una silueta apilada.

Arrastrar desde una zona vacía dibuja el mismo contorno negro/blanco que se usa
para redimensionar ventanas y selecciona los iconos que intersecta. Ctrl-click
agrega o quita elementos; Ctrl o Shift al iniciar el rectángulo conserva la
selección anterior. Ctrl+A selecciona todos. Escape cancela un arrastre. Los
grupos se pueden mover juntos, transferir a una carpeta/ventana del administrador
o enviar a la papelera. Cada archivo conserva los controles contra sobreescritura.

`tests/desktop/test_desktop_interaction.py` verifica organización, selección
aditiva, fallo de memoria al cambiar tamaños, transparencia de la silueta,
submenú en cascada y consumo seguro de transferencias múltiples.


Los tamaños de iconos se generan siempre desde una imagen fuente inmutable con
sus dimensiones originales. Los archivos del escritorio se cargan al tamaño
seleccionado, también cuando aparecen después de una transferencia. Cambiar
repetidamente entre 32, 48 y 64 no acumula pérdida ni mezcla anchos de imagen.
El administrador entrega una copia de la máscara alfa del icono mostrado al
iniciar el arrastre; conserva el logo de cada asociación y su transparencia.

Mover, copiar y eliminar abren una ventana de progreso con carpeta o papelera,
nombre del elemento y barra clásica. Un worker posee sus propias rutas y no
retiene punteros al administrador ni a sus entradas; la interfaz sigue libre.
Para árboles se cuentan elementos antes de operar, y al copiar también avanza
la fracción del archivo actual. El 100% se muestra tras finalizar correctamente;
los fallos se notifican y las vistas se actualizan incluso si el borrado fue
parcial. Los movimientos entre volúmenes conservan el origen como antes.

Las barras de herramientas conservan las medidas declaradas por cada programa.
El cálculo del mínimo de ventana no ensancha controles en función del orden de
creación; evita los botones pegados en Archivos, Editor y otras aplicaciones.
`test_desktop_interaction.py` cubre cambios repetidos de tamaño y la copia del
sprite de arrastre; `test_file_progress.py` comprueba trabajos diferidos, rutas
propias, progreso, operaciones recursivas y notificación de errores.
