# Componentes de terceros

Este archivo registra componentes de terceros identificados en el árbol; no
es una certificación exhaustiva de procedencia o permisos. Las
licencias de cada archivo tienen prioridad sobre la licencia MIT general del
proyecto.

El alcance general está en [NOTICE.md](NOTICE.md). La tabla de licencias,
excepciones y textos está en [LICENSES/README.md](LICENSES/README.md).
«Permiso no acreditado» describe un caso sin autorización demostrada; no es
una licencia ni un permiso concedido por BlesKernOS.

## Bibliotecas del navegador — MIT

El frontend propio de `programs/netsurf/` enlaza cuatro bibliotecas independientes
del proyecto NetSurf. Sus avisos completos están en:

- `programs/netsurf/vendor/libcss/COPYING` (J-M Bell).
- `programs/netsurf/vendor/libhubbub/COPYING` (J-M Bell).
- `programs/netsurf/vendor/libparserutils/COPYING` (J-M Bell).
- `programs/netsurf/vendor/libwapcaplet/COPYING` (The NetSurf Browser Project).

Los cuatro textos conceden MIT. No debe confundirse la licencia de estas
bibliotecas con la GPL del navegador NetSurf completo: este árbol contiene un
frontend y layout propios, según `programs/netsurf/README.md`.
Referencia upstream: https://www.netsurf-browser.org/projects/.

## BearSSL — MIT

`libs/bearssl/` contiene la biblioteca de Thomas Pornin. El aviso íntegro está
en `libs/bearssl/LICENSE.txt`. Se enlaza al proveedor TLS externo; la licencia
MIT del proyecto no sustituye el copyright ni el aviso de BearSSL.

## TinyGL — aviso recuperado y procedencia pendiente

`libs/src/` contiene TinyGL modificado, no código original íntegro de BlesKernOS.
El aviso de Fabrice Bellard, Gek y C-Chads se conserva en `libs/tinygl/LICENSE`;
es de tipo zlib con reconocimiento obligatorio en producto y documentación.
El alcance y las modificaciones se explican en `libs/tinygl/README.license.md`.
`libs/tinygl/UPSTREAM.json` fija una referencia upstream comprobada y hashes
locales; distingue esa referencia de la revisión histórica de importación,
que no se conoce. Los encabezados declaran constantes de Mesa sin identificar
la versión original. El aviso del Mesa 3.5 incluido se conserva también en
`LICENSES/Mesa-3.5-header.txt`, sin presentarlo como prueba de ese ancestro.
La consulta `glGetString(GL_LICENSE)` incluye el aviso TinyGL completo.

## Mesa 3.5 — licencias por componente

El inventario original está en `libs/Mesa-3.5/docs/COPYRIGHT`: el núcleo desde
Mesa 3.1 usa el aviso XFree86, mientras otros componentes conservan sus términos
individuales. `docs/COPYING` contiene el texto LGPL histórico; por sí solo no
describe la licencia de toda la distribución. Conservar ambos documentos y
los avisos por archivo al redistribuir el port.

## PL_MPEG — MIT

El decodificador de Dominic Szablewski está en
`programs/viewer/third_party/pl_mpeg.h`, con aviso separado en
`programs/viewer/third_party/PL_MPEG_LICENSE.txt`. La copia de
`extras/viewer/third_party/` conserva su propio aviso equivalente.

## TinyX, herramientas y aplicaciones opcionales

`ports/tinyx/README.md` distingue los avisos X.Org de los cambios GPL del fork
TinyCore. `ports/wine/package/` incluye también bibliotecas y runtimes con sus
licencias individuales; Wine no licencia todo ese paquete.
Las herramientas TCC, NASM, GNU make y binutils conservan sus textos al
empaquetarse mediante `tools/build_selfhost_toolchain.py`.

Los programas de `extras/` conservan licencias propias (por ejemplo, Freeciv,
Quake, Doom, ROTT y TuxRacer). La licencia del motor no concede automáticamente
permisos sobre los datos comerciales del juego. Los instaladores locales de
`media/installers/` y las imágenes externas de `tests/fixtures/` tampoco quedan
cubiertos por MIT ni se consideran source original del OS.

### Actua Soccer 96 — permiso no acreditado

No se encontró un LICENSE/COPYING de alcance general en
`extras/actuasoccer96-master/`. Hay avisos de HMI con derechos reservados, entre
otros en `SOURCE/NETNOW.H` y `SOURCE/NETDEFS.H`. La presencia de los fuentes no
acredita permiso de redistribución. Su licencia y la de los componentes HMI
no quedan acreditadas; el MIT de BlesKernOS no se les aplica. El alcance se
explica en `extras/actuasoccer96-master/LICENSE-NOTICE.md`.

### Heretic y Descent — casos distintos

El snapshot de `extras/heretic-master/` no conserva una licencia identificable
ni una revisión comprobada. `LICENSE-NOTICE.md` documenta ese estado; no se le
atribuye la GPL de otra publicación de Heretic por compartir nombre.

En cambio, `extras/descent-master/README.TXT` sí contiene una concesión propia
de Parallax, limitada a fines no comerciales, sin regalías ni ingresos.
`extras/descent-master/LICENSE-NOTICE.md` señala esa restricción. Esa copia
no se presenta como MIT/GPL. Los datos de ambos juegos tienen derechos separados.

### Avisos en las imágenes

Los empaquetadores ATA, actualización de usuario y comandos generan
`/DOCS/LICENSES.TXT` con este inventario y los textos completos seleccionados
por `tools/license_notices.py`, incluido TinyGL. Esto complementa los archivos
de fuentes/licencias específicos ya distribuidos; no resuelve los permisos
pendientes anteriores ni los de imágenes KDE/React95 indicados abajo.
Las imágenes existentes no se modifican al limpiar el source: deben
regenerarse o actualizarse para incorporar los avisos nuevos.

## Retrosmart — cursores clásicos

- Autor: Manuel Domínguez López; https://github.com/mdomlop/retrosmart-x11-cursors.
- Revisión y SHA-256 del archivo original: `third_party/retrosmart/UPSTREAM.json`.
- Licencia: GPL-3.0, conservada en `third_party/retrosmart/LICENSE`.
- Variante: bitmap blanca sin sombra, tamaño original 32×32 sin reescalado.
- 48 roles en `/SYSTEM/CURSORS`, incluidas animaciones WAIT y PROGRESS.
- Los archivos CUR/BCA convertidos mantienen GPL-3.0; la licencia MIT del OS
  no sustituye su licencia. Fuentes originales completas en
  `/SYSTEM/CURSORS/SOURCE.TGZ`, licencia en `LICENSE.TXT`, procedencia en
  `README.TXT` y `INDEX.JSON`, conversor en `BUILD.PY`.

## KDE 1 — recursos gráficos históricos

- Origen: repositorios oficiales `KDE/kde1-kdebase` y `KDE/kde1-kdelibs`.
- Revisiones fijadas: `third_party/kde1/*-source.json`.
- Recursos: 870 XPM/PNG válidos, 71 alias de compatibilidad y 142 variantes
  para controles de 16px/32px dentro de `GRAPHICS.PAK`.
- Transformación: conversión a PNG RGBA sin escalar; variantes separadas.
- Licencias: avisos originales conservados por módulo y carpeta; no se
  aplica la licencia MIT general de BlesKernOS a estas imágenes.
- Fuentes y avisos distribuidos con el OS: `/SYSTEM/KDE1SRC.TGZ` y
  `/SYSTEM/KDE1LIC.TXT`.

La procedencia individual no está verificada para todos los archivos.
`kdebase/kwm/LICENSE.readme` contiene términos Artistic propios; la GPL del
módulo no debe asignarse indiscriminadamente a todo el catálogo.
El README de `kdebase/kmenuedit/icons` reconoce imágenes de origen
indocumentado. El alcance y las exclusiones se registran en
`third_party/kde1/README.md`; incorporar el conjunto completo no equivale a
certificar que cada imagen tiene un permiso de redistribución comprobado.

## React95 icons — aviso de derechos de terceros

- Proyecto: React95, paquete `@react95/icons`.
- Origen: `https://github.com/React95/React95`, `packages/icons`.
- Versión del importador anterior: 2.5.3.
- Uso anterior: 975 PNG canónicos. Sustituidos por KDE 1 en
  `assets/graphics/GRAPHICS.PAK`; el importador opcional se conserva.
- Transformación: selección determinista de una variante por nombre y
  empaquetado sin alterar el PNG.
- Aviso: React95 distribuye su software bajo MIT, pero su licencia dice
  expresamente que Windows y todas las imágenes asociadas pertenecen a
  Microsoft Corp. y no están cubiertas por la licencia de React95.

En consecuencia, los iconos procedentes de React95 quedan excluidos de la licencia
MIT general de BlesKernOS y conservan los derechos de sus respectivos
titulares. El aviso original completo está en
`third_party/react95/LICENSE`.

## Wine — LGPL-2.1-or-later

El runtime Wine 11 se distribuye bajo `ports/wine/package/` y se integra
mediante la ABI Linux i386. Las fuentes, patches de integración y licencias
correspondientes se documentan en `ports/wine/README.md` y en los metadatos
del paquete. `third_party/wine/COPYING.LIB` conserva el texto de la LGPL.

La antigua implementación Win32/Win16 de `kernel/win32/`, el cargador NE y
sus metadatos derivados se retiraron del source activo en octubre de 2026.

## VMware SVGA / SVGA3D — MIT

- Proyecto/origen: interfaces públicas VMware SVGA mantenidas en el driver
  `vmwgfx` del kernel Linux.
- Archivos consultados: `svga_reg.h`, `svga3d_cmd.h` y `svga3d_types.h`.
- Uso en BlesKernOS: constantes, identificadores de comandos y estructuras
  binarias mínimas en `kernel/include/svga3d_protocol.h`.
- Código del driver y de las capas de transporte: implementación original para
  BlesKernOS; no se incorporó el driver Linux `vmwgfx` ni Mesa.
- Licencia de las definiciones públicas: `GPL-2.0 OR MIT`; BlesKernOS utiliza
  expresamente la concesión MIT incluida en los encabezados oficiales.

Copyright 1998-2021 VMware, Inc. Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated documentation
files, to deal in the Software without restriction, including without
limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, subject to preservation of the
copyright and permission notice. The Software is provided without warranty.

## ATI Rage 128 CCE/PM4 — MIT/X11

- Fuentes consultadas: manuales públicos de programación y registros de ATI;
  controlador DRM r128 histórico del kernel Linux; `xf86-video-r128` de
  X.Org/X11Libre; controlador DRI r128 histórico de Mesa.
- Archivos incorporados/adaptados:
  - `kernel/drivers/ati_rage128_cce_ucode.h`: microcódigo CCE de ATI publicado
    con el controlador DRM r128 y su aviso MIT/X11 completo.
  - `kernel/drivers/ati_rage128_3d_dvr.c`: implementación original para
    BlesKernOS basada en la disposición pública de registros y paquetes PM4.
- Uso: CCE 192PIO, triángulos transformados, color Gouraud y estados fijos 3D.
- No se incorporó DRM, DRI, AGP/GART ni el controlador Mesa completo.

Los avisos de copyright y permiso aplicables están preservados en el encabezado
que contiene el microcódigo. El resto del módulo es código nuevo para
BlesKernOS y conserva la licencia general del proyecto.

## Mesa Gallium 24.3.4 — backend VirGL

- Fuente: https://archive.mesa3d.org/mesa-24.3.4.tar.xz
- SHA-256: `e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025`.
- Integración: `ports/wine/graphics/build-gallium.py`, frontend OSMesa adaptado
  mediante `mesa-blesk-frontend.patch`, winsys propio `virgl-winsys.c`.
- Mesa conserva las licencias de sus componentes, principalmente MIT.
  El empaquetador conserva los textos de `licenses/` y los comentarios de
  copyright/licencia del código fuente en `MESA-GALLIUM-LICENSE.rst`.
- Los runtimes C++, GCC, Expat y zlib se acompañan de sus avisos Debian y los
  textos GPL/LGPL correspondientes, incluyendo las excepciones del runtime GCC.
- El backend se instala opcionalmente como `libbkvirgl.so.8`; Mesa 3.5
  conserva su distribución y licencia independientes.

## Colección de fuentes del sistema

Se redistribuyen 24 familias con sus originales TTF/BDF en `assets/fonts/source/`
y packs rasterizados BFN en `assets/fonts/`. El inventario completo, procedencia,
SHA-256 y licencia de cada original están en
[assets/fonts/catalog.json](assets/fonts/catalog.json).

DejaVu y Hack conservan sus avisos Bitstream Vera, Arev y adicionales. Ubuntu
usa la Ubuntu Font Licence; sus packs se llaman «Ubuntu derivative BlesK» y
«Ubuntu Mono derivative BlesK» según la regla de nombres para conversiones.
Helvetica Bold Bitmap, Helvetica Bold Oblique Bitmap, Courier Bitmap y Times Bitmap usan la licencia permisiva
Adobe/DEC X11 de `assets/fonts/licenses/X11-Adobe.txt`; los BDF originales se
conservan con sus avisos. Las otras familias usan SIL Open Font License. Los originales y los avisos se
conservan en el source; todos los textos se reúnen en
[assets/fonts/LICENSE.txt](assets/fonts/LICENSE.txt), redistribuido también en
`/SYSTEM/FONTS/LICENSE.TXT`.

## JBIG-KIT — impresión HP LaserJet 1020/1018

- Autor: Markus Kuhn, 1995–2014; https://www.cl.cam.ac.uk/~mgk25/jbigkit/.
- Copia upstream: OpenPrinting/foo2zjs; hashes y URLs en `third_party/jbigkit/UPSTREAM.json`.
- Licencia: GPL-2.0-or-later, conservada en `third_party/jbigkit/COPYING` y en cada archivo.
- PRINTSPL.BEX enlaza la biblioteca y se distribuye como ejecutable combinado GPL-2.0-or-later.
  Su adaptador local `system/services/print_zjs.inc` lleva la misma licencia.
- El kernel y los drivers residentes son ejecutables distintos y no enlazan JBIG-KIT.
- La imagen incluye licencia y fuentes de PrintSpool en `/DOCS/PRINTGPL.TXT`
  y `/DOCS/PRINTSRC.TGZ`, con Makefile autónomo para GCC i386.
- Fuente de texto raster: `libs/src/font8x8_basic.h`, Daniel Hepper/IBM,
  dominio público, avisos conservados. Texto ASCII y estilo aproximado.
- No se distribuyen binarios de firmware propietarios de HP.
