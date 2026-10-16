# TinyX/KDrive y Wine 11.0 en BlesKernOS

**Hito comprobado:** TinyX i386 atiende X11 sobre la GUI de BlesKernOS y
Wine 11.0 muestra un `MessageBoxA` real. La captura está en
[`evidence/messagebox.png`](evidence/messagebox.png), y el resultado del
gate KVM en [`evidence/messagebox-result.json`](evidence/messagebox-result.json).

## Fuentes y licencias

- [X.Org Server 1.2.0](https://xorg.freedesktop.org/archive/individual/xserver/xorg-server-1.2.0.tar.bz2),
  tarball oficial SHA-256
  `e3e56b35ee13098f4ee79948beb20bfc9a06d1a7a35fb906405ff1531b92bb85`.
  Los avisos originales se conservan en `upstream/xorg-server-1.2.0/COPYING`.
- [TinyCore TinyX](https://github.com/tinycorelinux/tinyx), revisión
  `feab72ca891bc04b18763763e15ee4e532369cdf`, basado en Xorg 1.2.0.
  Los cambios del fork están sujetos a GPLv3; se conserva
  `upstream/tinycorelinux-tinyx/COPYING`. Nuestros cambios en ese árbol
  (`kdrive/fbdev` y `os/access.c`) se distribuyen bajo GPL-3.0-or-later.
  Los avisos originales por archivo permanecen intactos.
- [libfontenc 1.1.8](https://xorg.freedesktop.org/archive/individual/lib/libfontenc-1.1.8.tar.xz),
  SHA-256 `7b02c3d405236e0d86806b1de9d6868fe60c313628b38350b032914aa4fd14c6`;
  [libXfont 1.5.4](https://xorg.freedesktop.org/archive/individual/lib/libXfont-1.5.4.tar.bz2),
  SHA-256 `1a7f7490774c87f2052d146d1e0e64518d32e6848184a18654e8d0bb57883242`.
  Sus avisos se conservan en los directorios fuente.

No se copiaron fuentes de otros sistemas operativos al kernel. La interfaz
`blesgfx.h` y su implementación en `kernel/linux/i386_ipc.inc` son código
escrito para BlesKernOS. Las pruebas de `tests/` son GPL-3.0-or-later.

## Qué hace el port

`/dev/blesgfx` da al DDX las operaciones para crear una ventana nativa de
760×536, presentar un framebuffer XRGB32 y recibir eventos GUI. Además, el
launcher puede seleccionar un viewport de esa pantalla para ajustar el
contenedor Vision a la aplicación Win32 visible. El kernel
valida/copia los píxeles desde memoria de usuario y la GUI los compone en una
ventana con propiedad del proceso TinyX. KDrive conserva su rasterizador X11;
`blesos.c` reemplaza la VT, el framebuffer Linux y la entrada Linux por esta
interfaz. `libX11.so.6` es la biblioteca cliente ELF i386 real, no el
prototipo SDK `sdk/lib/xlib.c`.

El framebuffer se revisa cada 33 ms y se publica cuando cambian sus pixeles
o la lista de ventanas. El teclado cubre letras,
modificadores, flechas, Inicio, Fin, RePág, AvPág, Insert y Supr; el mouse
entrega movimiento y botones a KDrive. La cola GUI tiene 64 eventos por
descriptor. Este port todavía no implementa modos variables, aceleración,
rueda, distribución completa de teclado ni todas las extensiones X11.

## Compilación

En un host con GCC multilib, Autotools, bibliotecas X11/Freetype i386 y
MinGW-w64:

```sh
TINYX_WORKDIR=/tmp/bles-tinyx-build ports/tinyx/scripts/build-i386.sh
```

El script copia las fuentes a una ruta sin espacios porque el libtool antiguo
no enlaza correctamente desde la ruta actual. Produce
`/tmp/bles-tinyx-build/prefix/XFBDEV`, ELF i386 con intérprete
`/WINEGATE/lib/ld-linux.so.2`. Las dependencias ELF van en `/WINEGATE/lib`
de la imagen guest; los tests se compilan con `gcc -m32` y los PE con
`i686-w64-mingw32-gcc`. Se incluyen en `tests/` los programas fuente.

## Verificaciones

- `scripts/build-i386.sh` completó desde cero y produjo un ELF i386 con
  intérprete `/WINEGATE/lib/ld-linux.so.2`; ese binario se usó en los gates
  finales (`SHA-256: 8dbc891b0d4fe451ce352943e967536607f13ac06f74a48c9ef6e9269cd2c409`).
- `Xfbdev -version` ejecutó dentro de BlesKernOS (`tinyx-version-gate-03`).
- El cliente i386 real ejecutó `XOpenDisplay`, `XCreateWindow`,
  `XFillRectangle` y recibió `Expose`; el rectángulo se ve en
  [`evidence/xlib-rectangle.png`](evidence/xlib-rectangle.png).
- Con Vision, VESA y PS/2 actuales, el cliente X11 recibió 16 teclas
  distintas, dos `MotionNotify` y un `ButtonPress` en KVM
  (`tinyx-stage1-current-vision-xlib/serial.log`).
- En KVM, AHCI y 1536 MiB, un PE de consola devolvió 0, `MessageBoxA` se
  mapeó y devolvió `IDOK`, y el launcher `wine-x11-launch` devolvió 0.
  Registro completo: `/home/lucas/tinyx-wine-messagebox-verified/serial.log`.

La prueba Wine utilizó una copia de un prefijo Wine 11.0 previamente
inicializado y validado por `w3-profile-baseline-reboot-05`. No demuestra
todavía una inicialización fresca de Wine con TinyX presente. Otra imagen
fresca emitió `run_wineboot boot event wait timed out`; se rechazó ese gate
aunque su launcher hubiese devuelto 0. En la prueba final ese marcador no
apareció.

## Etapa 1: aplicaciones Win32 esenciales

La salida de la etapa se comprobó con el kernel de 128 tareas, Vision, TinyX,
Wine 11.0, AHCI, KVM, 1536 MiB y dos CPU. Notepad permaneció abierto tres
minutos después de mapearse; recibió clic y escribió `blesk`, `wine` y
`swxa` completos. `Ctrl+A`, `Ctrl+C` y `Ctrl+V` duplicaron el texto;
`Ctrl+S` abrió «Guardar como». Después, en el mismo
arranque y sin pánico, se mapearon Regedit y Winemine; un clic en Winemine
reveló casillas y repintó la cuadrícula. Los tres lanzadores devolvieron 0;
el [resultado estructurado](evidence/stage1-vision-result.json) y el
[extracto del registro](evidence/stage1-vision-result.txt) conservan la
verificación de la corrida final.
Las capturas están en
[`stage1-vision-notepad.png`](evidence/stage1-vision-notepad.png),
[`stage1-vision-clipboard.png`](evidence/stage1-vision-clipboard.png),
[`stage1-vision-save-dialog.png`](evidence/stage1-vision-save-dialog.png),
[`stage1-vision-regedit.png`](evidence/stage1-vision-regedit.png) y
[`stage1-vision-winemine.png`](evidence/stage1-vision-winemine.png).

Por ahora la ABI Linux se fija a la CPU auxiliar cuando hay más de una CPU:
evita la corrupción de un frame de interrupción observada al migrar hilos de
Wine. Con una sola CPU, los servicios de Wine excedieron sus esperas y
Notepad no llegó a abrir. En una imagen de prueba que conservó un tamaño de
ventana anterior, Regedit restauró 760×542 y recortó su borde inferior;
en la corrida final abrió a 562×368 y cupo completo.
Una prueba dirigida adicional escribió `stage1` en Notepad, usó `Ctrl+S` y
comprobó en la partición FAT de la imagen que `STAGE1.TXT` contenía
exactamente esos seis bytes. Se conserva una
[copia del archivo guardado](evidence/stage1-vision-saved-file.txt).
Mover, redimensionar y minimizar ventanas Win32 dentro de TinyX todavía no
formaron parte de este gate; requieren pruebas dirigidas.

La continuación con aplicaciones externas, instaladores y dependencias X11
está en [Etapa 2](STAGE2.md).

### Rootless / Vision v2

El bridge expone top-levels X11 como ventanas Vision independientes. El XID se
usa como identidad estable, el kernel presenta un recorte del framebuffer
TinyX en cada ventana y remapea mouse/teclado al root X. Las acciones nativas
de cerrar, enfocar y redimensionar se devuelven al launcher para convertirlas
en operaciones X11. El root/container TinyX queda oculto en cuanto existe una
ventana rootless.

`wine-launch` también instala una paleta Win32 clásica aproximada a la paleta
CDE del escritorio. Esto no reemplaza USER32/GDI32 ni intenta re-tematizar
aplicaciones custom-drawn; sirve para que common controls y diálogos clásicos
se integren visualmente con el marco nativo.
