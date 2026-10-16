# GRAPHICS.PAK

`/SYSTEM/GRAPHICS.PAK` es el catálogo gráfico compartido de BlesKernOS. Evita
que cada aplicación incluya copias privadas de los mismos iconos y ofrece
nombres estables mediante las API públicas 22 y 23.

La imagen oficial usa el catálogo histórico KDE 1 de `kdebase` y `kdelibs`.
Incluye 870 bitmaps originales válidos, conservando todas sus variantes,
y 71 alias para los nombres que ya usa el OS: 1.083 entradas en total, incluyendo 71 variantes de 16×16 y 71 de 32×32.
Los recursos se convierten a PNG RGBA y se decodifican al usarlos.
Las familias `16/Nombre` y `32/Nombre` conservan los píxeles del dibujo: las
variantes mini se centran en un lienzo de 16×16, y las de 32×32 sólo eliminan
márgenes transparentes si hace falta. No se reducen dibujos grandes para
crear botones pequeños. `bk_gui_widget_set_icon()` elige la familia de 16px
globalmente; los botones dibujados a mano también la usan.
El escritorio inicia en tamaño chico, el Deskbar mide 52px de alto y sus
lanzadores y los iconos del Panel de control usan la familia de 32px.
El catálogo exacto está en `assets/graphics/GRAPHICS.CSV`; cada entrada
registra el archivo original.

Los nombres completos tienen la forma `KDE1/base/pics/folder` o
`KDE1/libs/toolbar/editcopy`. Las rutas largas llevan un sufijo de hash para
respetar los 47 caracteres del formato BKGP. Los alias como `Folder`, `Save`,
`Mouse` y `Keyboard` mantienen compatibilidad con las aplicaciones actuales.
Las API existentes permiten enumerar y cargar todas las entradas.

## API

```c
bk_gui_image_t icon;

if (bk_graphics_icon_load("Folder", &icon)) {
    bk_gui_surface_draw_image(surface,
        (bk_gui_rect_t){x, y, 32, 32}, clip, &icon);
    bk_gui_image_free(&icon);
}
```

- `bk_graphics_icon_load()` busca sin distinguir mayúsculas/minúsculas.
- `bk_graphics_icon_count()` devuelve la cantidad de entradas.
- `bk_graphics_icon_name()` permite enumerar el catálogo.
- La imagen cargada pertenece a la aplicación y siempre debe liberarse con
  `bk_gui_image_free()`.

Para botones estándar, la API 23 evita administrar la imagen manualmente:

```c
bk_gui_widget_t *save = bk_gui_create_button(
    desktop, window, bounds, "Guardar", on_save);
bk_gui_widget_set_icon(save, "Save");
```

El botón conserva y libera el recurso. Volver a llamar la función reemplaza el
icono; `bk_gui_widget_set_icon(save, NULL)` lo elimina.

Para regenerar el paquete fijado actualmente:

```sh
make graphics-pak
```

Para comprobar la estructura del paquete y todos sus píxeles usando el
decodificador PNG y el inflate reales del OS (requiere GCC y Pillow):

```sh
python3 tools/check_graphics_pak.py
```

La construcción KDE es local y requiere Python 3 y Pillow. Los originales
se recuperan, si hace falta, con `python3 tools/import_kde1_icons.py`, usando
los commits fijados en `third_party/kde1/*-source.json`.

El importador React95 anterior permanece disponible explícitamente con
`--download`, `--tarball` o `--png-dir`; no es el catálogo predeterminado.

## Licencia y procedencia

Los avisos y autores originales están en `third_party/kde1/`, junto a los
XPM/PNG originales. No se aplica la licencia MIT de BlesKernOS a estos
recursos. El catálogo completo conserva también imágenes con procedencia
pendiente de revisión: la licencia GPL general del proyecto no prueba la
licencia individual de cada imagen. Consulte `third_party/kde1/README.md`
y `kdebase/kmenuedit/icons/README` para las limitaciones documentadas.

La imagen del OS incluye `/SYSTEM/KDE1SRC.TGZ` con las fuentes, avisos,
metadatos y scripts de conversión, `/SYSTEM/KDE1LIC.TXT` con el resumen de
procedencia y `/SYSTEM/GRAPHICS.CSV` con el catálogo.
