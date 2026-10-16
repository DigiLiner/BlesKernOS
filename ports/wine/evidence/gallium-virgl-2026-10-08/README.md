# Gallium VirGL, integración 2026-10-08

Mesa 24.3.4, fuente oficial con SHA-256 fijado por build-gallium.py.
Pruebas ejecutadas en una copia del disco de usuario, sin reinstalar FIFA.

- `transport-host.log`: código de transporte del driver real, cola simulada,
  ASan/UBSan. Aislamiento de contextos y recursos, límites de transferencias,
  paquetes truncados, fences y limpieza: PASS.
- `transport-qemu.log`: QEMU/KVM, virtio-vga-gl, EGL headless sobre renderD128,
  1536 MiB y dos CPU. Contexto host independiente, textura 16x16, subida y
  lectura byte a byte: PASS. Este probe sustituye temporalmente el launcher
  solamente en la copia de prueba; por eso el arranque informa launcher-exit.

- `glsl-fbo-double-buffer-qemu.log`: GPU Intel UHD 600 del host a través de
  VirGL; OpenGL 4.3 Mesa 24.3.4. Shader GLSL 1.20 dibujado a FBO devuelve
  64,128,191,255 sin error. PASS contexto debug, textura compartida, superficies
  front/back independientes, copia de back a front en GPU y flush delantero.
- `glsl-fbo-depth-double-qemu.log`: profundidad 24 y stencil 8, clear y
  readback reales (0.25 / 5), sin errores.
- `glx-offscreen-pixmap-qemu.log`: front flush y back swap llegan a TinyX;
  superficie oculta resuelta por GPU se copia a pixmap persistente verde. PASS.
- `cold-cd-1.log` a `cold-cd-5.log`: cinco arranques en frío independientes
  con overlay temporal y el kernel corregido; CD ISO9660 montado 5/5.
- `audio-host.log`: PCM FLOAT, NaN/Inf, saturación, estéreo, frecuencia y cola. PASS.
- `fifa-before-*.log`: diagnósticos previos al soporte de flags, audio float y
  doble búfer; no son resultados finales ni pruebas de una partida jugable.

Estos resultados verifican el transporte y el renderizado programable real.
Falta validar por separado una partida completa de FIFA.


## Disco de usuario preparado

- Introducción, menú principal y selección de lado visibles por GPU:
  `fifa-intro.png`, `fifa-menu.png`, `fifa-side-select.png` y `fifa-menu-gpu.log`.
- Corregido el staging BFS4 del empaquetador: los destinos se normalizan a
  mayúsculas, incluyendo los componentes de directorio y los nombres copiados.
  Evita duplicar `prefix`/`PREFIX` y los alias del launcher en el host Linux.
- Reconstrucción completa independiente: 5456 entradas, BFS4 OK, sin duplicados.
- Recuperado `build/bleskernos-ata-user.img` desde
  `build/bleskernos-ata-user.pre-gallium.img` y aplicado el actualizador offline
  con `--fifa2000-gpu`. BFS4 OK: 5640 entradas, 70342 bloques libres.
- Comparados los archivos del respaldo con el resultado: ningún archivo
  eliminado ni cambiado dentro de las aplicaciones instaladas; launcher de
  producción y bibliotecas GL/VirGL coinciden byte a byte con el build.
- FIFA conserva su instalación y usa `"Thrash Driver"="dx6"`; Wine tiene
  `"EmulateModeset"="Y"`. Se conserva el respaldo anterior y el disco del
  intento fallido en `build/bleskernos-ata-user.failed-packaging.img`.
- La partida 3D queda pendiente de prueba manual por el usuario.

Desde la raíz del proyecto (la que contiene Makefile):

```sh
python3 tools/run_qemu_virtio.py --image build/bleskernos-ata-user.img --gl --disk-interface ahci --cdrom media/installers/FIFA2000.iso
```

Arranque final en snapshot: VirGL activo, CD montado, Wine READY en 16593 ms
y escritorio liberado (`user-final-boot.log`). VM de comprobación cerrada.


## Integración en builds normales

El Makefile declara Gallium, su cierre i386 y licencias como salidas agrupadas
con dependencias sobre el builder, winsys, frontend y ABI. ATA y `user-update`
las requieren; USB, VMware y los instaladores heredan el runtime de ATA.
`wine-gallium` usa las mismas salidas y deja de recompilar si están vigentes.
`user-update` también requiere el kernel actualizado antes de copiar stage2.
El actualizador directo exige el backend y todos sus avisos de licencia.

El launcher de QEMU selecciona SDL/OpenGL con `--gl`, mantiene GTK en 2D y
permite `--display gtk` explícito. La validación gráfica también usa SDL para
VirGL; los targets Virtio de Make aceptan `CDROM_IMG`. La normalización del
staging BFS4 impide las duplicaciones de nombres por diferencias de mayúsculas.

Evidencia SDL: `sdl-fifa-intro.png`, `sdl-fifa-host.log`, `sdl-fifa-guest.log`.
Resultado de integración y hashes: `normal-build-result.json`.


La prueba desde una imagen nueva detectó que la plantilla v4 importaba el tema
v5 durante el primer warmup y éste fallaba al intentar iniciar otro wineserver
(`normal-firstboot-before-fix.log`). El empaquetador ahora prepara el tema v5,
OSS e iconos offline, desde las mismas constantes del launcher, y crea el
marcador sólo después de escribir ambos registros. No modifica preferencias
al actualizar discos instalados. El helper pasó comprobaciones de contenido,
idempotencia y ausencia de datos de juegos instalados.


Validación final de la imagen normal: BFS4 OK, 5457 entradas, 74164 bloques
libres. Bibliotecas y nueve avisos de licencia comprobados byte a byte.
Gallium queda al día después de la compilación (`make -q wine-gallium`).
Arranques en snapshot con SDL/OpenGL y CD conectado:

- AHCI: Wine READY en 19646 ms y escritorio abierto (`normal-boot-ahci.log`).
- IDE predeterminado: Wine READY en 19750 ms y escritorio abierto
  (`normal-boot-ide.log`).
- Ambos logs de host sin EGL_BAD_ACCESS; ambas VM cerradas tras la prueba.
- Actualizador final aplicado sólo a la copia de prueba: 63 archivos de
  aplicaciones instaladas idénticos y launcher de producción restaurado.

El disco de usuario con instalaciones no se reconstruyó durante esta
integración. `build/bleskernos-normal-check.img` es la imagen limpia de
comprobación; no contiene los juegos del usuario.
