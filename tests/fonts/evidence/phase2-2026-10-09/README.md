# Segunda fase: 20 fuentes y visor clásico

`result.json` registra el catálogo, build User, pruebas del renderer real y
comprobaciones visuales en QEMU (2 CPU, 2 GiB, VirtIO sin GL).

Las capturas `dropdown-first.png` y `dropdown-last.png` muestran las 20 familias
con sus nombres renderizados en su propia tipografía. `large72.png` y
`large72-scrolled.png` muestran el ajuste de palabras y el desplazamiento tras
reducir la altura de la ventana. Las capturas iniciales de catálogo se tomaron
en el primer arranque; tamaños, selección con mouse, redimensionado y ajuste de
palabras se comprobaron en el segundo arranque de la build final.

Las acciones se inyectaron mediante el monitor HMP. Las capturas se revisaron
visualmente; no se usa OCR ni se afirma cobertura automática de toda la GUI.
La imagen de uso del usuario no se modificó.
