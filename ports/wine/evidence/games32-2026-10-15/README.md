# Juegos Win32: superficies OpenGL y runtime programable

Ronda exclusiva de i386, Wine 11.0 y Mesa 24.3.4. No agrega soporte Windows de 64 bits.

## Cambios

- El framebuffer de Gallium pertenece al drawable GLX, en lugar de al contexto. Dos ventanas/pbuffers del mismo tamaño conservan imágenes distintas; otros contextos pueden usar las mismas superficies.
- El frontend OSMesa local admite superficies diferentes para dibujar y leer. `glXMakeContextCurrent` ya no ignora `read`. Wine puede ejercerlo mediante `wglMakeContextCurrentARB`.
- Los drawables destruidos o reemplazados al redimensionar se liberan cuando termina la última vinculación. Un fallo de vinculación conserva el contexto anterior. El mutex protege el registro y sus referencias. Cuando lectura y dibujo usan la misma superficie, se consulta su geometría una sola vez.
- Gallium solicita y anuncia profundidad de 24 bits y stencil de 8. El backend clásico conserva su formato anterior. El backend clásico rechaza una lectura independiente que no puede representar.
- Se reconstruyó el runtime VirGL. La biblioteca de la imagen de entrada tenía una dependencia no distribuida (`libdrm.so.2`); `dlopen` fallaba y GLX acababa usando Mesa 3.5/OpenGL 1.2. La biblioteca reconstruida usa el transporte `/dev/blesgfx` y no necesita libdrm.
- El builder verifica recursivamente los ELF i386 y todos sus DT_NEEDED contra las bibliotecas realmente empaquetadas, sin resolver desde el host. GLX informa el motivo si falla la carga del backend programable.

Las tres funciones `OSMesaBlesk{CreateDrawable,DestroyDrawable,MakeContextCurrent}` son una interfaz privada entre los componentes locales GLX y Gallium. La API OSMesa existente conserva la propiedad de sus propios buffers. Los shaders, texturas y dibujos siguen pasando por VirGL; no se añadió un rasterizador CPU a esa biblioteca.

## Evidencia comparativa

| Prueba | Antes | Después |
| --- | --- | --- |
| Cargar VirGL desde la imagen de entrada | `glx-baseline3`: falta libdrm | `glx-final`: renderizado GPU |
| Alternar pbuffers A rojo / B azul y volver a A | azul, incorrecto | rojo |
| Dibujar verde en A y leer B azul | verde, incorrecto | azul |
| Otro contexto lee A verde | negro, incorrecto | verde |

`glx-semantics-before` agrega **sólo a la copia de diagnóstico** la dependencia libdrm faltante para aislar la semántica del framebuffer. Usa las bibliotecas GLX/Gallium anteriores. Esta dependencia no se agrega al producto; el backend nuevo no la necesita. `baseline` registra además el fallo Win32 anterior: OpenGL 1.2 y posterior excepción de acceso al iniciar DirectDraw.

`directx-verified` ejecuta un PE de 32 bits dentro del OS, mediante el launcher normal y TinyX: Direct3D 9 HAL, VBO dinámico, índices de 16 bits, textura managed, prueba de profundidad, shader HLSL ps_2_0, render-to-texture, lectura y muestreo de esa textura, restauración de state block, Reset con cambio de tamaño, conservación de textura/shader, 100 Present y consulta de evento GPU. También verifica OpenGL con profundidad 24/stencil 8, los pbuffers WGL y DirectDraw Lock/Blt/Lock. Todas esas comprobaciones pasaron; el launcher terminó con estado 0.

Los registros de la imagen distribuida están en `production-directx` y la repetición final `production-final`: toda la suite pasa y el launcher termina con estado 0, sin errores de fence/EGL en QEMU. Direct3D 11 crea un dispositivo HAL con feature level 11.0 (`b000`), compila shaders HLSL vs_4_0/ps_4_0 y dibuja un triángulo a una textura: el readback RGBA es 64,128,191,255. Esto verifica esas operaciones de D3D11; no certifica todas las funciones del nivel 11.0, tessellation o compute shaders.

`host-tests.log`: seis suites PASS. Incluyen ASan/UBSan en i386 y host para propiedad de drawables, lectura independiente, cambio de contexto, rechazo/rollback, resize y destrucción diferida; presentación, cierre ELF, transporte VirGL, rangos y barreras. Estas pruebas de host simulan el transporte; las lecturas de píxeles de los probes guest verifican la GPU real.

`notepad`: regresión gráfica en VMware, 1536 MiB y dos CPU. Notepad conserva teclado y copiar/pegar antes y después de reiniciar el compositor Ring 3: PASS.

Entorno guest de las pruebas 3D: QEMU/KVM, CPU host, 2 vCPU, 1536 MiB, IDE, virtio-vga-gl, EGL headless sobre `/dev/dri/renderD128`. Renderer observado: VirGL sobre Intel UHD Graphics 600 del host, OpenGL 4.3. Las duraciones host incluyen arranque, servicios y pruebas; no son FPS ni benchmarks de carga.

## Reproducción

Desde la raíz que contiene Makefile:

```sh
make -j2 wine-gallium
python3 tests/desktop/test_gallium_package.py
python3 tests/desktop/test_glx_drawables.py
python3 tests/kernel/run_wine_games32.py --base build/bleskernos-wine-games32.img --out build/my-games32-test
python3 tests/kernel/run_wine_games32.py --base build/bleskernos-wine-games32.img --native-glx --out build/my-glx-test
```

Cada runner usa una copia descartable y exige un directorio de salida nuevo. Requiere KVM, QEMU con VirGL/EGL, renderD128 accesible, GCC multilib y MinGW i686. `--local-gl` instala las bibliotecas del build únicamente en esa copia para iterar. La prueba rechaza fallos de fence/EGL registrados por QEMU.

Para probar programas manualmente:

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-wine-games32.img --gl --disk-interface ahci --cdrom media/installers/FIFA2000.iso
```

## Límites pendientes

- Los probes no certifican una partida completa ni todos los estados de Direct3D. Hace falta probar juegos concretos durante su uso, incluido audio y entrada.
- La aceleración programable comprobada corresponde a VirGL. El backend VMware actual y el Intel Gen2 del TravelMate conservan sus límites anteriores; esta ronda no incorpora Gallium SVGA ni shaders nativos Intel.
- La presentación todavía descarga color y lo envía a TinyX por X11. No hay presentación sin copias.
- La ronda anterior registró un fallo de fence del host en partidos largos. Estas pruebas cortas no bastan para declararlo resuelto.
- Wine emite durante Reset el diagnóstico `Something's still holding back buffer 0`. Reset devuelve éxito y la imagen nueva, la textura y el shader se comprueban después; el diagnóstico queda pendiente de investigar.
- La ABI Linux sigue sin sockets AF_INET. El paquete Wine continúa compilado sin Vulkan. Esta ronda no valida mandos ni DXVK.

Referencias de semántica: [GLX 1.4, Khronos](https://registry.khronos.org/OpenGL/specs/gl/glx1.4.pdf) y [recursos y Reset de Direct3D 9, Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3d9/lost-devices). Las capacidades del producto se sustentan en los probes y sus píxeles, no sólo en esos documentos.
