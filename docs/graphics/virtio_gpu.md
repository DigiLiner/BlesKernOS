# VirtIO GPU en BlesKernOS 0.9

Validación actual y corrección de corrupción de VBO: [VIRTIO_R128_VALIDATION.md](VIRTIO_R128_VALIDATION.md).

## Estado del controlador

`VIRTIOGPU.DVR` implementa el transporte PCI moderno de VirtIO GPU y se
integra con la ABI grafica 2D de BlesKernOS.

Funciones incluidas:

- deteccion PCI `1AF4:1050` y compatibilidad con `1AF4:1010` cuando expone
  las capacidades PCI modernas;
- negociacion obligatoria de `VIRTIO_F_VERSION_1`;
- colas split VirtIO en modo polling: `controlq` y `cursorq`;
- recurso 2D respaldado por RAM del guest;
- scanout 0, cambio de modo y presentacion por regiones sucias;
- comandos `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH`;
- fences sincronicos;
- cursor ARGB de hardware de 64 x 64;
- superficies 2D fuera de pantalla compatibles con la ABI de BlesKernOS;
- deteccion de `VIRTIO_GPU_F_VIRGL`, descarga de capsets y contexto VirGL;
- recursos 3D, attach/detach de contexto, transferencias 3D y `SUBMIT_3D`;
- autoprueba que crea un render target, ejecuta un clear VirGL y descarga el
  resultado antes de declarar operativo el transporte 3D.
- backend `gfx3d` para TinyGL: color interpolado, textura, mezcla alfa,
  profundidad Z16, vertex buffers y `DRAW_VBO`;
- tres VBO persistentes de 256 KiB, con rangos reservados hasta un fence real;
- vértices inline y acumulación de `DRAW_VBO` hasta el límite de 64 KiB por
  `SUBMIT_3D`, con división automática y sin reutilizar datos en vuelo;
- `surface_composite`, escalado y superficies de ventana para que el
  compositor mezcle capas 2D y viewports TinyGL directamente en VirGL;
- scanout directo de la superficie final VirGL, sin descargarla al framebuffer
  2D del guest cuando el host admite recursos 3D con `SCANOUT`;
- `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH` se publican juntos con una única
  notificación de virtqueue por repaint.

La presentacion 2D es paravirtualizada: el compositor dibuja en memoria del
guest y el controlador transfiere al host solamente la union de las regiones
modificadas. `fill_rect`, `bitblt` y las superficies de la ABI se mantienen en
software porque el protocolo VirtIO GPU 2D no define comandos de rasterizado
para rellenar o copiar rectangulos.

## Estado de la aceleracion 3D

El transporte VirGL y el backend `gfx3d` se validan de extremo a extremo. La
autoprueba crea superficies de color y profundidad, sube una textura y un
vertex buffer, enlaza shaders TGSI, ejecuta un triángulo con `DRAW_VBO` y exige
que el readback contenga el resultado renderizado.

Cuando la prueba termina correctamente el driver anuncia `GFX3D_CAP_TINYGL`.
TinyGL y 3D Plus envían sus triángulos transformados al backend; si QEMU no
ofrece VirGL o la prueba falla, conservan automáticamente el rasterizador CPU.

En el viewport interactivo, 3D Plus usa `bk_tinygl_present_gpu()` y registra la
superficie con `bk_gui_window_set_gpu_viewport()`. Esto termina el frame sin
readback y deja que el compositor la escale dentro de la ventana. La descarga
se reserva para el render previo, la exportacion BMP y otros consumidores que
realmente necesitan pixeles en CPU. Las texturas permanecen en la cache GPU
hasta que TinyGL las invalida o destruye el contexto.

## Compilacion

```sh
make build/system/drivers/VIRTIOGPU.DVR
```

El controlador queda en:

```text
build/system/drivers/VIRTIOGPU.DVR
```

`VIRTIOGPU.DVR` tambien forma parte de `DRIVER_OBJS`, por lo que se copia a la
imagen normal al reconstruirla.

## Prueba 2D en QEMU

```sh
make run-virtio-gpu
```

Equivale a usar la variante VGA de VirtIO, que conserva la compatibilidad VGA
para el arranque y despues cambia al scanout nativo:

```sh
qemu-system-i386 -cpu qemu32,+rdrand \
  -drive file=build/bleskernos-ata-user.img,format=raw,if=ide \
  -device virtio-vga -boot c -m 256M -serial stdio \
  -no-reboot -no-shutdown
```

## Prueba de negociacion VirGL

```sh
make run-virtio-gpu-gl
```

Comando equivalente:

```sh
qemu-system-i386 -cpu qemu32,+rdrand \
  -drive file=build/bleskernos-ata-user.img,format=raw,if=ide \
  -device virtio-vga-gl \
  -display sdl,gl=on \
  -boot c -m 256M -serial stdio -no-reboot -no-shutdown
```

La segunda prueba requiere que QEMU haya sido compilado con virglrenderer y
que el backend grafico del host admita OpenGL.

El launcher usa SDL con OpenGL para la ventana 3D. En este host, el arranque
con GTK llegaba al escritorio pero al abrir FIFA fallaba el contexto EGL con
`EGL_BAD_ACCESS`. SDL permitió mostrar el juego sin repetir ese error.
GTK sigue disponible explícitamente con `--display gtk` para diagnóstico;
el arranque Virtio 2D conserva GTK como predeterminado.

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-ata-user.img --gl --disk-interface ahci --cdrom media/installers/FIFA2000.iso
```

`make run-virtio-gpu-gl CDROM_IMG=media/installers/FIFA2000.iso` también conecta el CD.
Las reglas `run-*` construyen una imagen de distribución si está desactualizada;
para abrir el disco con programas ya instalados se usa el launcher directamente.
Para actualizar ese disco sin reinstalar, cerrar QEMU y ejecutar `make user-update`.

## Mensajes seriales esperados

```text
[VIRTIOGPU.DVR] registrado en PCI 0:2.0, prioridad=240
[VIRTIOGPU.DVR] modern PCI listo: queues=2 scanouts=1 virgl=no edid=si
[VIRTIOGPU.DVR] scanout 0: 800x600x32 pitch=3200 recurso=1
```

Con VirGL disponible tambien se enumeran entradas similares a:

```text
[VIRTIOGPU.DVR] capset[0]: id=1 version=... size=...
[VIRTIOGPU.DVR] VirGL transporte listo: capset=1 v... bytes=... hash=... ctx=1 submit3d=OK
[VIRTIOGPU.DVR] VirGL GFX3D/TinyGL: DRAW_VBO + Z + textura + descarga OK
```

## Diagnostico

- `dispositivo ... no detectado`: QEMU no expuso VirtIO GPU.
- `transporte moderno ausente`: el dispositivo solo expuso una interfaz
  legacy, que este controlador no utiliza.
- `dispositivo sin VIRTIO_F_VERSION_1`: se rechazo una interfaz no moderna.
- `timeout en controlq`: revisar bus mastering, MMIO, identidad entre direccion
  virtual/fisica y la configuracion de QEMU.
- `virgl=no`: la ruta 2D funciona igual; solo indica que no existe transporte
  VirGL en ese arranque.
- `submit3d=fallo seguro`: el contexto o el renderer rechazó una operación 3D;
  el controlador conserva la ruta 2D y no anuncia aceleración de triángulos.

Si la composicion o el scanout 3D son rechazados por el host, el controlador
restaura automaticamente el recurso 2D y conserva el camino de compatibilidad.
