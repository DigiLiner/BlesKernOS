# Rediseño de administración — 2026-10-09

`visual-processes.png`, `visual-performance.png`, `visual-services.png` y
`visual-startup.png` muestran el estilo clásico con relieve, columnas y barra de
estado. La captura de rendimiento usa cuatro CPU, todas en la mitad superior.

`visual-filtered-events.png` muestra el filtro estricto por PID al abrir desde
Process Manager. `visual-events-icon.png` muestra la terminal completa;
`visual-event-errors.png`, el filtro de errores y los puntos rojos.
`visual-event-search*.png` muestra búsqueda desde un diálogo separado.
`visual-desktop-arrange.png` verifica la organización automática desactivada.

`host-tests.log` registra nueve pruebas del backend y de las funciones reales.
`boot-64m-1cpu.log` y `boot-256m-2cpu.log` corresponden a imágenes aisladas BFS4,
con las aplicaciones ejecutándose en Ring 3. El runner comprueba el contenido
pintado de las ventanas, además del backend y el retorno a espacio de usuario.

Repetir desde la raíz del proyecto:

```sh
python3 tests/kernel/run_admin_smoke.py --memory 64 --cpus 1 --output tests/kernel/evidence/ui-redesign-2026-10-09
python3 tests/kernel/run_admin_smoke.py --memory 256 --cpus 2 --output tests/kernel/evidence/ui-redesign-2026-10-09
python3 tools/build_admin_image.py
```

La imagen para probar manualmente es `build/bleskernos-admin.img`.
