# ATI Rage128: estado y pruebas físicas

## Estado

`ATIR1283D.DVR` apunta específicamente a ATI Rage Mobility M3/LF
**1002:4C46**, con `ATIR128.DVR` activo en 32 bpp. No se afirma compatibilidad
con todas las ATI Rage. El usuario confirmó arranque y 3D funcional en la Dell,
pero informó líneas de colores, UI trabada y aproximadamente 2 FPS en el cubo
de 3D Plus. La nueva auditoría y sus cambios todavía requieren prueba física:
[diagnóstico de rendimiento y protocolo de medición](ATI_R128_PERFORMANCE_AUDIT.md).

El motor es CCE/PM4 192PIO, con microcódigo, coordinación con el driver 2D y
esperas acotadas. La configuración actual incluye Z16 experimental activado.
El driver sólo se declara operativo después de sus autopruebas de píxeles.

## Cambios para el contrato común de Mesa/GFX3D

- La propiedad del motor dura cada operación, no todo el intervalo lógico
  `begin/draw/end`. Mesa puede dejar un frame abierto mientras interviene la GUI.
- Cada draw público se divide en paquetes de hasta 384 vértices. Un lote de
  3072 vértices ya no se rechaza por superar el límite de un paquete.
- Se mantienen 64 handles de superficies. El presupuesto real de VRAM no
  aumenta: el asignador conserva sus reservas y límites y Mesa usa expulsión LRU.
- Destruir una textura también espera sus lectores pendientes; la espera ya
  no se limita al render target actual.
- Alpha test: referencia de 8 bits y traducción explícita de las ocho
  comparaciones de GFX3D a la enumeración distinta de Rage128.
- DECAL mezcla RGB con el alpha de textura y conserva el alpha interpolado.
- Blend premultiplicado usa factor de origen ONE.
- Mipmaps nativos: hasta 11 niveles, offsets alineados a 32 bytes y registros
  ordenados desde el mip más pequeño, siguiendo el controlador histórico.
- Se conserva presentación VRAM → VRAM mediante el motor 2D compartido.

El driver no anuncia `ORDERED_SUBMIT`. Antes de soltar el motor después de
cada draw público, espera que terminen CCE y la escritura de su caché de
píxeles. Entonces devuelve un fence completado. `end` mantiene su comprobación
síncrona. Esto añade una espera por lote público, no por triángulo ni por cada
fragmento de 384 vértices.

## Revisión anterior de sincronización

La corrupción de Virtio no se traslada literalmente a ATI: `submit_vertices`
copia las palabras del buffer CPU al FIFO PIO antes de reutilizarlo; la
GPU no conserva un puntero a esa RAM. No necesita el anillo de VBO de VirGL.

Sí había un riesgo equivalente al cambiar de propietario: después de enviar
PIO, el draw soltaba el motor con rasterización pendiente. El puente 2D
puede detener PM4 al entrar, antes de que el cliente 3D llame a `end`. La
espera por lote anterior evita esa interrupción y garantiza la visibilidad
de los píxeles antes de permitir operaciones 2D o reutilización de VRAM.

Además, el coordinador usa ahora el `kmutex` recursivo y atómico del kernel:
desactivar la preempción de un CPU no protegía frente a otro núcleo. La
retirada de superficies conserva ese bloqueo hasta liberar sus páginas.
La autoprueba de destinos intercalados acepta y comprueba los fences ya
completados. Estos cambios se verificaron por lectura del código y
compilación; no se ejecutó QEMU ni la tarjeta para esta revisión.

## Autopruebas para la tarjeta

Además del triángulo texturado con Z16, el arranque comprueba:

- 24 casos de alpha test (8 comparaciones, 3 referencias).
- DECAL y conservación del alpha.
- Selección de mip y desactivación del filtro mip.
- Destinos intercalados y un draw de 18000 vértices dividido en paquetes.

Buscar `[R128:TEST] ... PASS` y el diagnóstico `[ATIR1283D:TRACE]`.
Si alguna prueba falla, el backend 3D no se habilita. Ante timeout se detiene
CCE y se desactiva la ruta acelerada. No interpretar compilación o revisión
teórica como un `PASS` de hardware.

## Límites todavía presentes

- Las texturas admitidas por el rasterizador son ARGB8888, potencias de dos,
  con base entre 8 y 1024 texels por eje. No hay soporte general para NPOT.
- `surface_composite` de ATI sigue sin implementarse; no se anuncian escalado,
  transformación ni superficies de ventana para el compositor GPU. Tux puede
  dibujar en la GPU y aun necesitar readback para la composición de su ventana.
- No hay multitextura, stencil, shaders programables ni AGP/GART.
- El usuario midió aproximadamente 2 FPS en el cubo de 3D Plus. La causa
  dominante aún necesita los tiempos de la nueva instrumentación; no se
  atribuye ese rendimiento simplemente al límite de VRAM ni a PIO.

## Prueba sugerida

1. Usar la imagen USB normal recompilada `build/bleskernos-usb-user.img`,
   o reconstruir un disco de prueba con
   `python3 tools/build_svga3d_validation.py --output build/ati-test --perf`.
   Ese kernel inicia Tux y autodetecta también ATI; los discos de prueba
   anteriores no contienen esta revisión final.
2. Arrancar en 800×600×32 y guardar el log serie completo.
3. Confirmar el ID PCI, los `PASS`, y `renderer=ati_rage128_3d`.
4. Ejecutar `perfmon watch`, abrir Tux, jugar, hacer click y mover/cerrar/reabrir
   ventanas. Revisar colores, texto, profundidad y continuidad del escritorio.
5. Conservar los contadores `[ATIR1283D:PERF]`, `[R128:ENGINE]`, `PERF:APP`,
   `PERF:GUI` y los readbacks. Comparar el mismo recorrido y resolución.

## Fuentes de revisión

- [Mesa 7.11.2, estado r128](https://raw.githubusercontent.com/notaz/mesa/mesa-7.11.2/src/mesa/drivers/dri/r128/r128_state.c): `r128UpdateAlphaMode`.
- [Mesa 7.11.2, combinadores y mipmaps](https://raw.githubusercontent.com/notaz/mesa/mesa-7.11.2/src/mesa/drivers/dri/r128/r128_texstate.c).
- [Mesa 7.11.2, offsets de niveles](https://raw.githubusercontent.com/notaz/mesa/mesa-7.11.2/src/mesa/drivers/dri/r128/r128_texmem.c).
- [Registro de Xorg r128](https://raw.githubusercontent.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-r128/master/src/r128_reg.h).

Estos enlaces son copias del código histórico de los proyectos, no documentación
de generaciones Radeon posteriores. Las copias usadas durante la revisión están
en `build/virtio-validation/references/`.
