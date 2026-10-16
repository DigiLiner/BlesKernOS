# Índice de licencias

El MIT de [`../LICENSE`](../LICENSE) corresponde al código original del OS.
[`../NOTICE.md`](../NOTICE.md) define su alcance y las excepciones.
Las licencias de los archivos y componentes de terceros tienen prioridad para
esos materiales. La presencia de un texto GPL/MIT en un directorio no demuestra
por sí sola que cubra cualquier archivo sin atribución de ese directorio.

## Componentes identificados

| Componente | Licencia / alcance | Texto y evidencia local |
|---|---|---|
| Código original de BlesKernOS | MIT, salvo avisos específicos | `LICENSE` |
| libcss, Hubbub, libparserutils, libwapcaplet | MIT de cada biblioteca; no confundir con el navegador NetSurf completo | `programs/netsurf/vendor/*/COPYING` |
| BearSSL | MIT, Thomas Pornin | `libs/bearssl/LICENSE.txt` |
| TinyGL modificado | Aviso propio de tipo zlib con reconocimiento obligatorio; revisión de importación desconocida | `libs/tinygl/LICENSE`, `README.license.md`, `UPSTREAM.json` |
| Mesa 3.5 | Núcleo XFree86/MIT; otros componentes según sus archivos | `libs/Mesa-3.5/docs/COPYRIGHT`, `COPYING`, encabezados individuales |
| PL_MPEG | MIT, Dominic Szablewski | `programs/viewer/third_party/PL_MPEG_LICENSE.txt` |
| Wine | LGPL-2.1-or-later para Wine; el paquete contiene dependencias con otros términos | `third_party/wine/COPYING.LIB`, `ports/wine/README.md`, `ports/wine/package/` |
| TinyX | X.Org por archivo y GPL para cambios del fork TinyCore | `ports/tinyx/README.md`, `ports/tinyx/upstream/*/COPYING` |
| Mesa Gallium y runtimes | Por componente, con excepciones GCC donde corresponda | `ports/wine/package/wine-11.0-i386/licenses/` |
| Retrosmart y conversiones CUR/BCA | GPL-3.0 según metadatos upstream | `third_party/retrosmart/LICENSE`, `UPSTREAM.json`, fuentes/conversor distribuidos |
| Fuentes TTF/BDF y packs BFN | OFL, Ubuntu Font Licence, Bitstream/Arev y Adobe/DEC según familia | `assets/fonts/catalog.json`, `assets/fonts/LICENSE.txt` |
| PrintSpool + JBIG-KIT | Ejecutable combinado GPL-2.0-or-later | `third_party/jbigkit/COPYING`, `system/services/print_zjs.inc` |
| E100 / plantilla eepro100 | GPL-2.0-only para el módulo derivado | `kernel/drivers/network/e100.c`, `third_party/linux-eepro100/COPYING` |
| PCI IDs | BSD-3-Clause | `third_party/pciids/COPYING` |
| Definiciones VMware SVGA | Concesión MIT de los encabezados de licencia dual | `kernel/include/svga3d_protocol.h`, inventario general |
| Microcódigo ATI Rage 128 | MIT/X11 del aviso incorporado | `kernel/drivers/ati_rage128_cce_ucode.h` |
| TCC, NASM, GNU make, binutils | Términos específicos, no MIT global | Avisos instalados por `tools/build_selfhost_toolchain.py` |

## Casos que no se presentan como redistribución verificada

| Material | Estado concreto | Evidencia / aclaración |
|---|---|---|
| Actua Soccer 96 y componentes HMI | Permiso general no acreditado; hay derechos reservados | `extras/actuasoccer96-master/LICENSE-NOTICE.md` |
| Snapshot local de Heretic | Publicación y licencia no acreditadas | `extras/heretic-master/LICENSE-NOTICE.md` |
| Descent | Concesión identificada con restricciones no comerciales | `extras/descent-master/README.TXT`, `LICENSE-NOTICE.md` |
| Catálogo KDE 1 completo | Licencias por módulo y archivo; incluye procedencias no acreditadas | `third_party/kde1/README.md`, especialmente `kdebase/kmenuedit/icons/README`; `kwm/LICENSE.readme` tiene términos Artistic propios |
| Imágenes Windows de React95 | Excluidas expresamente del MIT de React95 | `third_party/react95/LICENSE`; ya no son el catálogo gráfico predeterminado |
| Datos comerciales, medios y marcas | Derechos separados de la licencia del motor o del OS | Avisos de cada juego; `media/installers/` y fixtures no se relicencian |

«Permiso no acreditado» no equivale a dominio público, licencia libre ni una
autorización concedida por este proyecto. Estos casos requieren permiso
documentado o reemplazo para poder presentarlos como redistribución autorizada.
Esta clasificación no cambia qué archivos se conservan o qué tema usa el OS.

## Aplicaciones opcionales de extras

| Aplicación | Términos identificados / límite |
|---|---|
| Doom, Quake, ROTT, Freeciv, TuxRacer, GLtron, gl-117, Duke Nukem 3D | GPL según los avisos originales de cada árbol; no incluye automáticamente los datos comerciales ni toda dependencia |
| OpenMRac | Código BSD-2-Clause (`LICENSE`); datos CC0-1.0 (`media-data/LICENSE`) |
| 3D Engine / 3D Plus | Adaptaciones locales y TinyGL/Mesa con sus términos; assets con avisos Kenney/OpenGameArt en `assets/` |
| Viewer | Código local y decodificador PL_MPEG MIT con aviso propio |
| Codex32, HyperZip, LexiNet, Bles3D-Core | El MIT original sólo alcanza el código del autor del OS; conservar los avisos de las dependencias vendorizadas. Contenido descargado por LexiNet conserva los términos de su fuente |
| Actua, Heretic, Descent | Ver las excepciones concretas de la tabla anterior |

Los textos originales de cada aplicación se mantienen en sus directorios.
No se asigna una versión GPL sólo por encontrar una copia genérica de GPL:
se debe atender a la concesión en los archivos que se distribuyen.

## Obligaciones y empaquetado

Conservar copyrights, textos y atribuciones; indicar los cambios locales;
mantener los nombres exigidos por las licencias de fuentes; y, para los
componentes copyleft, acompañar los binarios con las fuentes correspondientes
y medios de reenlazado cuando lo exija su licencia. No sustituirlo por un
enlace a fuentes upstream que no representen las modificaciones distribuidas.

`tools/license_notices.py` reúne avisos en `/DOCS/LICENSES.TXT`, incluido este
índice y `NOTICE.md`. Este archivo complementa, no reemplaza, las fuentes y
avisos específicos enviados en el disco. El empaquetador de source conserva
`LICENSE`, `NOTICE.md`, `THIRD_PARTY_LICENSES.md` y este directorio.

Verificación local: `python3 tests/licenses/test_notices.py` comprueba los textos
íntegros, los hashes TinyGL de la revisión y la consulta real `GL_LICENSE`.

El inventario detallado y la procedencia están en
[`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md).

## Fuentes de drivers e impresión

- `kernel/drivers/network/e100.c` y E100.DVR: GPL-2.0-only. La plantilla de
  configuración procede de Linux eepro100 y conserva sus atribuciones.
- `system/services/printspool.c` y PRINTSPL.BEX: GPL-2.0-or-later por JBIG-KIT.
- `third_party/pciids/` y `kernel/drivers/graphics/families/*_vbe_ids.inc`:
  BSD-3-Clause; copyright Martin Mares y Albert Pool.
- OHCI, CDC-ECM y el puente EHCI nuevos son implementaciones originales MIT.

El LICENSE MIT general del proyecto no sustituye las licencias específicas
anteriores ni las de otros paquetes de terceros. Las referencias están en
`docs/platform/family-driver-sources.json`. Los fuentes correspondientes y
Makefiles de E100/PrintSpool se incluyen en FAMILYSRC.TGZ/PRINTSRC.TGZ del disco.
