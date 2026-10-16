# Fuentes del sistema

Se incluyen 24 familias con sus TTF/BDF originales en `source/` y packs BFN listos
para el kernel. `catalog.json` registra nombres, archivos, procedencia, hashes
SHA-256 y licencia de cada original. Los avisos completos están en `licenses/`
y se reúnen en [LICENSE.txt](LICENSE.txt), también instalado en el OS.

DejaVu Sans, DejaVu Serif, DejaVu Sans Mono, Noto Sans, Noto Serif, Noto Mono, Hack, Ubuntu derivative BlesK, Ubuntu Mono derivative BlesK, Roboto, Open Sans, Montserrat, Oswald, Comic Neue, Arimo, Cousine, Rubik, Inter, Manrope, DM Serif Display.

Ubuntu y Ubuntu Mono llevan el sufijo «derivative BlesK» en los packs y el visor,
como exige su licencia para esta conversión de formato. Los TTF originales se
conservan sin modificaciones. Las fuentes variables se rasterizan en peso 400;
cuando existe un eje óptico se usa 14, dentro del rango admitido.

Las builds normales consumen los BFN existentes, sin requerir Pillow ni
FreeType en el host. Para regenerarlos:

```sh
make fonts-regenerate
# O una sola familia del catálogo:
python3 tools/build_fonts.py --family Roboto
```

La regeneración requiere Python 3 y Pillow con FreeType. Los packs actuales
se generaron con Pillow 10.2.0 y FreeType 2.13.2; otras versiones pueden cambiar
ligeramente los píxeles. Cada tamaño entero entre 8 y 72 px se rasteriza desde
el TTF, con cobertura de 8 bits y métricas propias.

Los 24 packs comprimidos ocupan unos 93 MiB. El kernel carga y descomprime
solamente los tamaños solicitados. El OS recibe los BFN y la licencia en
`/SYSTEM/FONTS`; los originales quedan disponibles en el source.

Ver [uso, API y formato](../../docs/desktop/fonts.md).

Helvetica Bold Bitmap, Courier Bitmap y Times Bitmap provienen de las fuentes
Adobe/DEC de X11, con diseños y avisos de copyright de 1984–1989 y 1994. Se
conservan seis tamaños BDF originales por familia (8, 10, 12, 14, 18 y 24 px),
sus hashes y la licencia X11-Adobe. La distribución actual incluye extensiones
Unicode posteriores: no se presenta todo ese repertorio como original de 1994.
Los tamaños originales se copian sin cambiar sus píxeles; los otros se obtienen
del tamaño nativo más cercano usando vecino más próximo, sin interpolación.
El renderer nativo dibuja todas las familias sin antialiasing (umbral 128 para
los packs antiguos con cobertura gris). Esto no cambia el renderer de Wine.

Helvetica Bold Oblique Bitmap agrega la variante inclinada original de X11;
el botón «Start» usa esta variante con texto gris claro (`#C0C0C0`).

Para ampliar las bitmap se prefieren factores enteros 2×/3× cuando existe un
tamaño original adecuado. «Start» elige tamaños nativos o múltiplos exactos y
se dibuja sin sombra desplazada para conservar los píxeles uniformes.
