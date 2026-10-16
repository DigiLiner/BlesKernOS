# Mouse global y controles comunes

La sensibilidad es un estado del nucleo que se conserva al registrar drivers.
Los paneles consultan ese estado y la estela del escritorio; abrir uno no
recarga ni reaplica copias de MOUSE.INI. Cada control modifica solo su campo.

La orientacion de relieve compartida esta en bleskernos_theme.h; ventanas,
widgets y paneles la consumen. Mouse usa pestañas, grupos y botones comunes.
La zona de prueba recibe eventos reales, marca clics y cuenta los tres
botones y dobles clics izquierdos.

Pruebas host: handlers de dos instancias, persistencia, error de escritura,
reemplazo de driver, pares de doble clic, botones y limites de la zona.
Rasterizacion: pestañas comunes y orientacion del relieve hundido.
QEMU: 256 MiB, 2 CPUs, TCG, mouse PS/2; capturas de ambas pestañas, tres
botones, un doble clic y seleccion de velocidad 5. Se verifica MOUSE.INI
en el disco aislado despues de cerrar QEMU.

La imagen build/bleskernos-admin.img tiene kernel, stage2, Mouse y controles
actualizados. No se actualiza la imagen de usuario mientras QEMU la tiene
abierta. tools/update_user_image.py conserva esa comprobacion e incluye
Process Manager y la documentacion del tema al actualizar en reposo.

Repetir:

```sh
python3 tests/desktop/test_mouse_properties.py
python3 tests/kernel/test_classic_theme.py
python3 tests/desktop/test_visuals.py
python3 tests/desktop/run_mouse_smoke.py
```
