# Etapa 2: aplicaciones de escritorio Win32

## Banco de pruebas

La base es la imagen FAT32 con Wine 11.0 y prefijo inicializado de la Etapa 1.
Se ejecuta en QEMU/KVM con `-cpu pentium3 -smp 2 -m 1536`, AHCI, Vision y
TinyX 760×536. `scripts/prepare-stage2-image.py` clona esa imagen, incorpora
un PE i386 o MSI externo y agrega las cinco bibliotecas X11 opcionales que
`winex11` intenta cargar (`libXrender`, `libXfixes`, `libXcursor`, `libXi` y
`libXrandr`). También puede lanzar un ejecutable ya instalado con
`--run-installed`. No se incluyen binarios de aplicaciones externas en el
repositorio.

Para validar cambios del kernel en una imagen desechable, se ejecuta
`scripts/build-stage2-kernel.py IMAGEN` después de preparar la imagen. El
script construye un kernel de prueba con un ejecutor que lanza
`/WINEGATE/bin/wine-stage1-launch`, actualiza Stage 2 y limpia el relleno del
último sector para que coincida la suma de comprobación del arranque.

El cliente X11 confirmó que TinyX anuncia `RENDER`, `SHAPE`, `XFIXES` y
`RANDR`. `XInputExtension` no está disponible. Las DLL de Wine 11.0
`comctl32`, `comdlg32`, `shell32`, `ole32` y `msi` están empaquetadas; su
presencia por sí sola no valida sus operaciones.

## Primeras aplicaciones externas

| Aplicación | Prueba | Estado |
| --- | --- | --- |
| PuTTY 0.85 i386 | Ventana de configuración, dos minutos abierta | Pasó el gate gráfico. Guardar una sesión y conectar por SSH aún no están validados. [Captura](evidence/stage2-putty.png). |
| 7-Zip 9.20 i386, instalador EXE | Instalación desde el asistente | La pantalla final confirmó la instalación y el proceso salió con código 0. Persisten `7zFM.exe`, `7z.exe`, `7z.dll`, la ruta y la entrada de desinstalación en el registro. [Captura](evidence/stage2-7zip-setup-finished.png). |
| 7-Zip File Manager | Arranque desde la instalación persistida | Abrió una ventana de 562×368 y permaneció dos minutos; launcher 0, sin pánico. [Captura](evidence/stage2-7zip-file-manager.png). |
| 7-Zip, lectura y extracción | Abrir `STAGE2.ZIP` y extraer `stage2.txt` | El archivo apareció en la lista, se abrió el diálogo de extracción y FAT recibió 24 bytes con el contenido esperado. [Archivo abierto](evidence/stage2-7zip-archive.png), [diálogo](evidence/stage2-7zip-extract-dialog.png), [archivo extraído](evidence/stage2-7zip-extracted.txt). |
| IrfanView 4.62 i386 | Instalar, reiniciar la imagen y abrir una PNG | El asistente mostró «Installation successful!» y persistieron `i_view32.exe`, complementos y la entrada de desinstalación. El visor abrió la PNG de 320×200 mediante File → Open y la dibujó correctamente. [Captura](evidence/stage2-irfanview-png.png). |
| 7-Zip 9.20 i386, instalador MSI | `msiexec /i`, licencia y asistente | Tras implementar `utimensat`, terminó con pantalla de éxito y código 0. FAT conserva `7zFM.exe`, DLL, archivos de idioma y sus fechas; el registro conserva las entradas de 7-Zip. En un arranque posterior, File Manager abrió y permaneció dos minutos con launcher 0. [Asistente](evidence/stage2-7zip-msi-fixed.png), [File Manager](evidence/stage2-7zip-msi-file-manager.png). |

En esa corrida EXE, el launcher antiguo devolvió 4 porque consideraba fallo
cualquier salida antes del tiempo de espera, aunque el instalador terminó con
código 0. El launcher ahora admite `--accept-exit` para evaluar instaladores.

PuTTY proviene de la [publicación oficial 0.85](https://www.chiark.greenend.org.uk/~sgtatham/putty/releases/0.85.html)
(`w32/putty.exe`, SHA-256
`4c70267ca03a00ea761ec358498b990a7221decb36eace9b7dbe6a751be0fb3b`).
7-Zip 9.20 proviene de la [página oficial de descargas](https://www.7-zip.org/download.html)
(`7z920.exe`, SHA-256
`1e2f2a8fb52d3972b9b65b8ad1bebb66965c47a2994f89b3d652c31e6f6e4c3c`;
`7z920.msi`, SHA-256
`fe4807b4698ec89f82de7d85d32deaa4c772fc871537e31fb0fccf4473455cb8`).
IrfanView 4.62 proviene del enlace de SourceForge de la [página oficial de descargas](https://www.irfanview.com/download_sites.htm)
(`iview462_setup.exe`, SHA-256
`37cdb372c4b6053356eca2c40aa44f4fb8cd30681c28cda54e80601d6c7b565a`).

## Límites detectados

- La capa Linux de sockets ofrece `AF_UNIX` de tipo stream. `AF_INET` y
  datagramas aún no están implementados; no hay una prueba SSH válida de
  PuTTY ni cobertura de Winsock/DNS/TLS.
- Wine informa `start_rpcss Failed to start RpcSs service` durante el
  arranque. El impacto en MSI, COM y OLE debe medirse con pruebas dirigidas.
- El primer intento MSI registró `ACTION_InstallFiles compressed file wasn't
  installed ("_7zFM.exe")` y terminó en 1603 ([captura anterior](evidence/stage2-7zip-msi-failed.png)).
  Una traza de `msi` y `cabinet` mostró que el CAB se descomprimía, pero
  `SetFileTime` recibía `ENOSYS` de `utimensat` antes del callback que marca
  cada archivo como instalado. La ABI Linux i386 ahora implementa
  `utimensat` y `utimensat_time64` sobre las fechas FAT. El MSI terminó
  correctamente en la repetición. Siguen faltando imágenes del asistente
  WiX, un problema visual separado.
- En IrfanView, el botón final del asistente quedó por debajo del área visible
  de TinyX. La instalación mostró éxito y dejó los archivos utilizables, pero
  se cerró con Alt+F4 y código 96; la salida limpia del asistente no está
  validada. Al lanzar el visor, la PNG pasada como argumento no apareció de
  inmediato; la apertura mediante File → Open sí funcionó.
- La entrada automatizada necesita esperar a que Wine procese los eventos de
  control; una captura inmediata después de un clic no demuestra que el
  control haya cambiado de foco.
- La configuración probada usa dos CPU; el caso de una sola CPU sigue fuera
  de la aceptación vigente.

La salida de la Etapa 2 requiere instalar y usar varias aplicaciones externas
de forma persistente. Una ventana que se abre constituye sólo un primer gate.
Los resultados medidos se conservan en
[stage2-result.json](evidence/stage2-result.json).
