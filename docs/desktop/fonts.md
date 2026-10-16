# Fuentes reales y visor Font

BlesKernOS incluye 24 familias: DejaVu Sans, DejaVu Serif, DejaVu Sans Mono, Noto Sans, Noto Serif, Noto Mono, Hack, Ubuntu derivative BlesK, Ubuntu Mono derivative BlesK, Roboto, Open Sans, Montserrat, Oswald, Comic Neue, Arimo, Cousine, Rubik, Inter, Manrope, DM Serif Display, Helvetica Bold Bitmap, Courier Bitmap, Times Bitmap y Helvetica Bold Oblique Bitmap. Los TTF/BDF
originales están en `assets/fonts/source/`; las builds User y Developer
instalan sus packs BFN y la licencia en `/SYSTEM/FONTS`, y el visor en
`/SYSTEM/PROGRAMS/FONT.BEX`.

Abrir `font` desde Ejecutar, usar `start font` en Terminal o elegir Font en el
menú de aplicaciones.
Las imágenes nuevas incluyen el acceso «Fuentes» en el escritorio. El visor usa
un panel gris con relieves, campos hundidos y separación entre configuración y
vista previa. El desplegable muestra cada nombre con su propia fuente y permite
recorrer las 24 familias mediante su barra de desplazamiento. A su lado hay un
campo de tamaño editable con flechas. La vista previa inferior adapta las líneas
al ancho disponible y permite desplazarse para leer muestras grandes.

- `F4` o `Alt` + flecha abajo: abrir/cerrar el desplegable.
- Flechas, `Inicio`, `Fin`, `RePág`, `AvPág`: recorrer la lista abierta.
- Una letra: buscar la siguiente familia cuyo nombre comienza con ella.
- `Enter`: aceptar; `Esc`: cancelar la elección o edición.
- `Tab` / `Shift+Tab`: recorrer fuente, tamaño y vista previa.
- `[` / `]`: cambiar familia; `-` / `+`: cambiar tamaño.
- Rueda, barra vertical o teclas de página: desplazar la vista previa.


Se admiten todos los tamaños enteros de **8 a 72 píxeles em**. El alto de línea
puede ser mayor: incluye ascendentes y descendentes. Cada tamaño se rasteriza
directamente desde el contorno TrueType mediante FreeType en el host, conservando
sus métricas; el dibujo nativo aplica un umbral y no suaviza los bordes; el OS usa operaciones enteras y no interpreta TTF.
La API consume UTF-8. El repertorio inicial contiene 201 caracteres: ASCII
imprimible, Latin-1 (acentos, ñ, ü, ç), euro, comillas tipográficas, guiones,
viñeta, puntos suspensivos y carácter de reemplazo. Los caracteres ausentes se
muestran como `?`. No hay shaping, kerning ni soporte completo de Unicode.

La tipografía bitmap de los controles existentes se conserva. Las aplicaciones
pueden usar las fuentes reales con la API nueva; instalarlas no cambia la
configuración tipográfica de Wine, que tiene su propio motor y sus fuentes.

## API nativa, versión 34

Incluir `bleskernos_api.h`; los metadatos comunes se definen en
`bleskernos_font.h`. Se agregan cinco símbolos sin alterar los anteriores:

- `bk_font_count()`: cantidad de familias válidas encontradas.
- `bk_font_info(id, &info)`: nombre, ID, tamaños mínimo/máximo y flag monoespaciada.
- `bk_font_line_height(id, size)`: alto de línea, o cero si no se puede cargar.
- `bk_font_text_width(id, size, text, length)`: ancho lógico en píxeles.
- `bk_font_draw_text(surface, id, size, x, y, text, length, color, clip)`:
  dibuja texto sin suavizado y devuelve si pudo cargar la fuente.

Los IDs permanecen estables durante el arranque. Se deben consultar los nombres;
no usar IDs fijos. `length` es una cantidad de bytes, con un máximo de 4096;
la lectura termina antes si encuentra NUL. `x,y` indican el inicio de la línea,
no su baseline. `\n` avanza de línea, `\r` se ignora y `\t` ocupa cuatro espacios.
La medición de varias líneas devuelve el ancho de la más larga, saturado a
65535. La medición y el dibujo usan los mismos avances en unidades de 1/64 px.

```c
bk_font_info_t info;
for (uint32_t id = 0; id < bk_font_count(); ++id) {
    if (!bk_font_info(id, &info)) continue;
    const char sample[] = "¡Hola, BlesKernOS!";
    bk_font_draw_text(surface, id, 24, clip.x + 8, clip.y + 8,
                      sample, sizeof(sample) - 1, 0x00203040, clip);
    break;
}
```

La caché global mantiene ocho pares fuente/tamaño generales y otros 24 bloques
pequeños de 16 px para los nombres del desplegable (hasta 128 KiB cada uno).
Descarta los menos recientes y está protegida para SMP. Cada bloque tiene un
máximo de 2 MiB después de descomprimir.
Los archivos, índices y offsets se validan antes de acceder a los píxeles.
El recorte intersecta la región del cliente, el clip de la superficie y sus
dimensiones reales de almacenamiento, incluidos origen y pitch.

Para instalar otra familia, generar un BFN compatible y copiarlo a
`/SYSTEM/FONTS`; reiniciar para actualizar el catálogo (máximo 64 familias).
No alcanza con copiar un TTF al invitado. Para incluirla en las builds, agregar
su original a `assets/fonts/source`, su entrada en `assets/fonts/catalog.json` y su
BFN a `assets/fonts`, siempre con la licencia de redistribución correspondiente.

`make fonts-regenerate` recompila los packs con Pillow/FreeType. Las builds
habituales usan los packs ya preparados y no requieren esas dependencias.
`make user-update` instala las fuentes y Font también en la imagen existente;
conserva el escritorio configurado por el usuario, por lo que allí puede abrirse
Font desde Ejecutar o el menú de aplicaciones.

## Formato BFN1 / BFN2

Todos los campos multibyte son little-endian; las estructuras no tienen padding.

| Sección | Contenido |
| --- | --- |
| Header, 64 bytes | `magic[4]="BFN1"`, versión u16=1 o 2, cantidad de tamaños u16, nombre UTF-8 terminado en NUL `char[48]`, flags u32 (bit 0=mono), reservado u32=0 |
| Índice, 16 bytes/tamaño | tamaño em u16, alto de línea u16, ascendente i16, reservado u16=0, offset absoluto u32, longitud del bloque u32 |
| Bloque | cantidad de glifos u32, registros ordenados por codepoint, mapas de cobertura |
| Glifo, 20 bytes | codepoint u32, offset relativo al bloque u32, ancho u16, alto u16, bearing izquierdo i16, bearing superior relativo al baseline i16, avance 26.6 u32 |
| Cobertura | ancho × alto bytes por glifo; 0=transparente, 255=opaco; filas consecutivas |

La versión 1 guarda cada bloque sin compresión. La versión 2 conserva el magic
`BFN1` y los mismos índices, pero el bloque almacenado comienza con su longitud
descomprimida u32, seguida de secuencias RLE. Cada tag representa
`(tag & 127) + 1` bytes: si el bit 7 está activo, se repite el byte siguiente;
si está apagado, se copian los siguientes bytes literalmente. Los offsets de
los glifos se refieren al bloque descomprimido. El lector acepta ambas versiones
y rechaza truncamientos, desbordamientos y longitudes inconsistentes.

Los tamaños del índice son consecutivos y crecen de uno en uno. El generador
usa el motor BASIC de Pillow y no aplica ligaduras ni kerning entre glifos.

## Verificación

```sh
python3 tests/fonts/test_font_pack.py
python3 tests/fonts/test_real_fonts.py --preview /tmp/font-preview.png
python3 tests/fonts/test_font_image.py
```

La prueba de packs valida las 24 familias, sus originales y las secuencias RLE.
La prueba del renderer compila y ejecuta el C real con adaptadores de VFS,
AddressSanitizer y UndefinedBehaviorSanitizer: 24 familias, 65 tamaños,
UTF-8, métricas proporcionales/mono, caché, recorte y archivos corruptos.
La prueba de imagen instala los packs en BFS4 y comprueba su integridad y la conservación
de un archivo del usuario. La vista previa se genera con ese mismo renderer.

La primera fase cuenta con [evidencia histórica](../../tests/fonts/evidence/2026-10-09/result.json).
La ampliación a 20 familias y el nuevo visor se verifican por separado en
[evidencia de la segunda fase](../../tests/fonts/evidence/phase2-2026-10-09/result.json).

## Fuentes bitmap históricas y Start

Las cuatro familias Adobe/DEC de X11 se generan desde BDF originales, sin pasar por
contornos TrueType. En tamaños nativos se mantienen los píxeles originales; los
otros tamaños usan vecino más próximo. Ver los detalles de procedencia en
`assets/fonts/catalog.json` y la licencia completa en `assets/fonts/licenses/X11-Adobe.txt`.

La barra de inicio muestra «Start» en Helvetica Bold Oblique Bitmap, centrado y adaptado
al espacio del botón. Se conservan sus estados de pulsado y la apertura del menú.
Todos los textos de esta API se dibujan sin antialiasing; Wine conserva su propia
configuración tipográfica. Los packs existentes con cobertura de 8 bits siguen
siendo compatibles y se dibujan aplicando un umbral de 128.

[Capturas y validación de las bitmap y START](../../tests/fonts/evidence/retro-2026-10-09/result.json).

Para ampliar las bitmap se prefieren factores enteros 2×/3× cuando existe un
tamaño original adecuado. «Start» elige tamaños nativos o múltiplos exactos y
se dibuja sin sombra desplazada para conservar los píxeles uniformes.

## Dibujo con estilos, API 38

`bk_font_draw_text_styled(surface, id, size, x, y, text, length, color, clip,
style, letter_spacing, word_spacing)` conserva la geometría de la API anterior
y permite combinar `BK_FONT_STYLE_BOLD`, `BK_FONT_STYLE_ITALIC` y
`BK_FONT_STYLE_ANTIALIAS`. Negrita y cursiva son sintéticas; el suavizado usa la
cobertura del pack, sin volver a rasterizar contornos. Los espacios adicionales
se expresan en píxeles y admiten −8 a 64. UTF-8 sigue limitado a 4096 bytes.

El ancho lógico sigue usando `bk_font_text_width`; al solicitar negrita debe
reservarse un píxel adicional por bloque de texto. Para espaciado explícito,
agregar `(caracteres − 1) × letter_spacing + espacios × word_spacing`. El
sesgo de cursiva puede sobresalir del ancho lógico; `clip` limita la tinta.
La función anterior `bk_font_draw_text` conserva su salida monocromática.
NetSurf usa la función nueva para texto web y el título del documento.

`bk_font_text_advances(id, size, text, length, advances, capacity)` devuelve
un avance por carácter UTF-8, en unidades de 1/64 de píxel. `capacity` es la
cantidad de elementos `uint32_t` disponibles (1–4096); devuelve la cantidad
escrita o cero si la fuente no carga o el vector no alcanza. Tab equivale a
cuatro espacios; CR y LF tienen avance cero. El caller administra los saltos
de línea y puede conservar estas métricas para medir sin repetir syscalls.
Las funciones de fuentes conservan su mutex interno y no toman el dominio
general LEGACY del kernel.
