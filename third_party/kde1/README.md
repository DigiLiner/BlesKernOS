# KDE 1: catálogo gráfico para BlesKernOS

Fuentes: copias históricas oficiales de KDE, `KDE/kde1-kdebase` y
`KDE/kde1-kdelibs`. Los commits y URL exactos están en `*-source.json`.
Son repositorios históricos adaptados para compilar en sistemas modernos;
no son una copia byte por byte de los tarballs de KDE 1.0 de 1998.

Se conservan todos los XPM y PNG de ambos módulos y sus avisos originales.
El paquete convierte todos los bitmaps válidos a PNG RGBA de 8 bits por
canal sin escalar ni cambiar colores. Conserva cada variante por separado.
Los alias de `aliases.json` permiten usar los nombres existentes de
BlesKernOS. `Mouse` y `Keyboard` identifican imágenes distintas.

Exclusiones del paquete (los originales permanecen aquí):

- `kdebase/compat/`: seis imágenes de Chromium y Qt Creator añadidas por el
  port moderno; no pertenecen al catálogo histórico.
- `kdebase/kappfinder/pics/xftp.xpm`: contiene un Desktop Entry, no un XPM.
- `kdebase/kfm/pics/info.xpm`: utiliza el color `o` sin definirlo.
- `kdebase/kwm/pics/menu.xpm`: contiene filas truncadas.

## Licencias y autores

Estos recursos NO están cubiertos por la licencia MIT de BlesKernOS.
Conservan las licencias, copyrights y autores del upstream. Se incluyen
`COPYING`, `LICENSE.readme`, `AUTHORS`, `CREDITS`, `README` y otros avisos
junto a las imágenes en sus ubicaciones originales.

La licencia general de kdebase es GPL v2; algunos componentes tienen
licencias diferentes. Por ejemplo, `kdebase/kwm/LICENSE.readme` conserva
términos Artistic propios. No debe interpretarse como un permiso individual
verificado para cada imagen. En particular,
`kdebase/kmenuedit/icons/README` atribuye un grupo a Greg J. Badros bajo GPL,
otro a Ludvig A. Norin (1995) y otro a Christoph Neerfeld bajo GPL, pero
reconoce que desconoce la procedencia de los restantes iconos de esa carpeta.
Los iconos de aplicaciones de terceros y marcas conservan sus derechos.
Este catálogo completo está disponible para desarrollo; la revisión de
procedencia de todos sus archivos sigue pendiente antes de redistribuirlo
como un conjunto íntegramente verificado.

El estado del catálogo es, por tanto, **licencias por componente con permisos
individuales no totalmente acreditados**, no «todo GPL» ni «todo MIT».
Los grupos identificados en `kmenuedit/icons/README` no cubren los iconos cuyo
origen el propio README desconoce. Las fuentes y avisos se conservan; no se
añaden permisos a esos archivos mediante esta documentación.

`assets/graphics/KDE1SRC.TGZ` contiene estas fuentes, los avisos originales,
los metadatos, los alias y los scripts de conversión. Se instala junto al
paquete en `/SYSTEM/KDE1SRC.TGZ`; este aviso se instala como
`/SYSTEM/KDE1LIC.TXT`. No se altera el copyright de los originales.

## Reconstrucción

Con Python 3 y Pillow instalados, `make graphics-pak` reconstruye el paquete
sin red. Para recuperar los originales fijados por commit:

```
python3 tools/import_kde1_icons.py
make graphics-pak
```

`x11-colors.json` registra los valores RGB de los nombres de colores X11
que requieren los XPM históricos. No se necesita una instalación de X11
para reconstruir el catálogo.

## Variantes nativas para controles

`small-aliases.json` elige por función las variantes mini existentes.
Los recursos `16/Nombre` las centran sin escalarlas en un lienzo transparente
de 16×16 (algunos originales son 16×14 o menores). Los recursos `32/Nombre`
centran dibujos de hasta 32px, recortando sólo bordes transparentes si los
originales tienen 34×34. Se conservan sin modificar los originales y sus
recursos `KDE1/...`. Los controles estándar nunca reducen un icono grande.
