# FIFA 2000: resultado de la ronda de texturas

**El parpadeo de FIFA 2000 sigue sin resolverse.** Se reprodujeron texturas
incorrectas y polígonos gigantes durante partidos reales en QEMU/KVM con
virtio-vga-gl. La captura `fifa-still-corrupt.png` muestra el fallo con la
versión conservada. No hay una imagen certificada que corrija FIFA.

## Corrección verificada e independiente

Se corrigió una carrera al reutilizar búferes de vértices: el winsys espera
a los lectores GPU pendientes antes de sobrescribir el búfer. Una espera
posterior a la escritura no protege las lecturas anteriores. Se usan subidas
con fence, conservando los envíos asíncronos de comandos.

El caso nativo de reutilización de VBO pasó de 536/1024 píxeles incorrectos
a 0/1024. También pasaron los casos de recursos nuevos y mapas con descarte.
Esto verifica la carrera aislada; no demuestra que explique el fallo de FIFA.

## Validación

- `native-verified/`: GLX nativo, presentación, contextos, pbuffers y tres casos
  de orden de búferes; resultado PASS, sin errores GPU registrados.
- `directx-verified/`: Wine i386, D3D7, D3D9, D3D11, OpenGL/WGL y DirectDraw;
  resultado PASS. D3D7 dibujó 4096 quads y verificó 1024 píxeles sin diferencias.
- `host-tests-verified.log`: cinco suites de búferes, secuencias, rangos,
  transporte y paquete; PASS.

FIFA continuó mostrando corrupción con subidas cercadas, sincronización de
cada envío y una variante sin el atajo de enlace de contexto GLX. No se
conservan esas variantes experimentales como solución. La causa restante
no se ha determinado. Los snapshots anteriores y las capturas de investigación
son históricos; la evidencia verificada está identificada arriba.

La imagen original `build/bleskernos-wine-tuned.img` permanece sin modificar.
`build/fifa-texture-investigation/candidate-v4.img` es una copia de prueba
con el backend verificado, no una solución certificada para FIFA.
