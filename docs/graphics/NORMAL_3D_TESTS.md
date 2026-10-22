# Pruebas 3D incluidas temporalmente en las builds normales

Las imágenes ATA, USB y VMware incluyen glmark2 y las demos Gears, Gloss y
Morph3D. El inicio normal del escritorio se conserva: no se ejecutan benchmarks
automáticamente.

También incluyen el protector **Medusa (Gallium)**, seleccionable en las
propiedades de pantalla. Reutiliza la escena original de glmark2 en una ventana nativa sin bordes
que ocupa el escritorio, con un limite de 30 FPS. Se cierra con el mouse
o una tecla y espera a que GLMARK2.ELF libere sus recursos antes de volver.
Si no puede abrir la escena muestra un mensaje de error especifico sobre
fondo negro. No utiliza un renderer por software como sustituto.
Se puede abrir directamente con `/SYSTEM/SCREENS/SSJELLY.SCV`.

En Ejecutar se pueden introducir estas rutas y argumentos.

## VirtIO/VirGL

```
/SYSTEM/PROGRAMS/GLMARK2.BEX
/SYSTEM/PROGRAMS/MDGEARS.BEX
/SYSTEM/PROGRAMS/MDGLOSS.BEX
/SYSTEM/PROGRAMS/MDMORPH3D.BEX
```

Los lanzadores usan Gallium por defecto. Glmark2 necesita el dispositivo
VirtIO con VirGL (`--gl` en `tools/run_qemu_virtio.py`).

## Mesa 3.5 acelerado, también para SVGA3D

```
/SYSTEM/PROGRAMS/MDGEARS.BEX --backend mesa35 --size 640x480
/SYSTEM/PROGRAMS/MDGLOSS.BEX --backend mesa35 --size 640x480
/SYSTEM/PROGRAMS/MDMORPH3D.BEX --backend mesa35 --size 640x480
```

Este modo exige la ruta acelerada disponible; no sustituye un fallo con
renderizado por software. VMware conserva la ruta Mesa 3.5/SVGA3D existente:
todavía no hay un port de Gallium SVGA. Incluir las demos no certifica el
rendimiento ni la disponibilidad de aceleración en cada tarjeta física.

Se puede comparar con CPU usando `--backend software`. Para pruebas cortas,
las demos admiten `--seconds 20`. Escape cierra las demos.

Las correcciones de presentación están incorporadas, pero glmark2 todavía
registra pausas y la escena de refracción ronda los 28 FPS en la VM medida.
Estas builds no implican una nueva aprobación de rendimiento 3D.
