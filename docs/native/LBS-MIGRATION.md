# Migración de aplicaciones y CPL a LBS

Esta segunda entrega mueve las bibliotecas directamente a `/SYSTEM`:
`/SYSTEM/SYSTEM.LBS`, `/SYSTEM/VISION.LBS`, `/SYSTEM/NGS3D.LBS`, etc.
El instalador elimina las copias anteriores de esas bibliotecas en
`/SYSTEM/LIBS/NATIVE`, sin eliminar bibliotecas ajenas. El cargador acepta la
antigua ruta absoluta como alias de la nueva, para clientes recompilados con
este cargador. Un ejecutable antiguo que incorpora su propio cargador anterior
no recibe esa corrección automáticamente: debe reenlazarse.

## Aplicaciones migradas

El manifiesto `sdk/lbs/applications.json` contiene **140 clientes**: 36 GUI,
98 comandos, tres salvapantallas, dos lanzadores Wine y Gears para developer.
Las rutas, fuentes y destinos de cada uno son explícitos. El empaquetador
selecciona esos bytes antes de crear el archivo temporal que copia al disco.
`tests/lbs/test_payload_selection.py` protege esa ruta de producción.

Los objetos originales de `build/programs` pueden ser intermedios de compilación;
los instalables están en `build/lbs/apps`. No copiar los intermedios para instalar
la migración. Cada instalable tiene un JSON con sus proveedores e importaciones.

Extracciones efectivas:

- MEDIA compila los únicos decodificadores canónicos de `libs/native/media`.
  Viewer utiliza handles opacos, metadatos, seek, streaming AVI/MPEG y audio;
  ya no enlaza vv_*, vr_* ni pl_mpeg privados.
- VISION contiene los widgets comunes extraídos del reproductor.
- NGS contiene el raster de imágenes escaladas y tablero de transparencia.
- ARCHIVE contiene BKZ1 y ZIP/DEFLATE/CRC de HyperZip. Compress, Extract,
  Archive Manager e HyperZip llaman a esas implementaciones.
- COMMAND compila una vez los helpers de `system/commands/common.c`.
- NGS3D y TINYGL suministran los motores compartidos de los salvapantallas,
  Gears y las variantes 3D de los extras.

NetSurf mantiene sus asignaciones nativas en un único dominio mediante un
adaptador explícito. Los archivos de runtime `LIBLBSPX.A` y `LIBLBSNT.A`
aíslan los helpers de descriptor POSIX del API nativo `bk_file_size/write`.
Esto evita que el puente termine llamando a una función con otro tipo de fd.
Los archivos históricos del SDK siguen disponibles para compatibilidad.

Las excepciones arquitectónicas figuran en `sdk/lbs/exceptions.json`:
recuperación puede funcionar sin LBS; kernel y drivers mantienen privilegios;
VISION.BEX y PRINTSPL.BEX son proveedores de servicio y no cargan sus propios
clientes de IPC. Windows/Linux conservan sus ABI.

Los extras se declaran por separado en `sdk/lbs/extra_applications.json` y se
construyen con `make lbs-extras`. Sus proyectos producen por defecto clientes
LBS; los objetivos raw son sólo entradas intermedias para el migrador. Las
fuentes locales indicadas por el usuario tienen prioridad sobre la copia inicial
de GitHub; `sdk/lbs/external-source.json` registra la procedencia. Freeciv
mantiene su nombre y las entradas separadas de cliente y servidor.

## Cómo se enlaza y se conserva el ciclo de vida

`generate_lbs_client.py` inspecciona los símbolos externos del objeto de la
aplicación, y resuelve los disponibles en las bibliotecas. Cambia únicamente
esas referencias del cliente por saltos indirectos cdecl. No modifica las
importaciones nativas del cargador o de su puente al kernel: esto evita que una
llamada desde una LBS vuelva recursivamente al mismo cliente.

El punto de entrada abre las bibliotecas necesarias, verifica ABI y resuelve
todos los símbolos antes de llamar al programa. Si falta un módulo o símbolo,
registra el error, libera el prefijo abierto y termina con estado 127 sin ejecutar la aplicación.
No hay fallback silencioso al código estático.

Los módulos quedan fijados durante la vida del proceso. Algunas aplicaciones
retornan del punto de entrada con hilos o callbacks registrados, y otras salen
mediante syscall sin retornar; descargar al retornar sería incorrecto. Al
finalizar el proceso se libera su espacio de direcciones y recursos. El código
y las constantes de las LBS ahora comparten páginas físicas; los datos siguen
siendo privados. Véase [la actualización de memoria](LBS-SHARING.md).

Para consumidores heredados se añadieron únicamente los símbolos explícitos
listados en `sdk/lbs/legacy_exports.json`. Cada uno reenvía a la misma entrada
Ring 3 que el ejecutable anterior utilizaba, preservando su firma. No se
convirtieron llamadas privadas del kernel en direcciones accesibles desde usuario.
Las cabeceras nuevas siguen siendo la interfaz preferida para programas nuevos.

## Wine y aceleración

Se inspeccionaron `ports/wine/graphics/glx.c`, `gfx3d-linux.c`,
`virgl-winsys.c`, el adaptador de TinyX y sus reglas de compilación.
La ruta 3D actual es, según el renderer:

```text
Aplicación Windows: Direct3D/OpenGL
  -> Wine / wined3d
  -> libGL.so.1 (adaptador GLX local)
  -> libbkvirgl.so.8 (Gallium/VirGL), libbkmesa.so.1 o libOSMesa.so.8
  -> interfaz Linux /dev/blesgfx
  -> driver gráfico del sistema
```

La idea de traducir llamadas Windows al backend gráfico del sistema es correcta,
y esa traducción ya existe. Las LBS de esta entrega usan la ABI nativa del SDK;
Wine utiliza ELF/Linux, glibc, TLS y sus propios contextos GLX. No puede cargar
NGS3D.LBS como sustituto directo de una biblioteca Linux. Habría que desarrollar
y probar un adaptador ABI específico. Sustituir Gallium por Mesa 3.5 nativo
podría perder shaders, extensiones y compatibilidad que ya utiliza Wine.

Por eso **se conserva el backend 3D de Wine**. El lanzador nativo WINE.BEX y su
CPL sí usan SYSTEM.LBS, pero eso no cambia dónde renderiza Wine. El 2D de Wine
pasa por GDI/X11/TinyX y presentación a Vision; tampoco se agregó una conversión
extra a NGS. Mejoras del driver común pueden beneficiar ambas rutas sin forzar
una dependencia LBS donde no corresponde.

## Construcción e instalación

```sh
make -j6 lbs-apps lbs-audit
python3 tests/lbs/audit_clients.py
python3 tools/generate_lbs_docs.py
python3 tools/install_lbs.py
# Imagen: build/bleskernos-lbs-user.img; original preservada.
```

Para cambiar la lista de clientes, actualizar `applications.json`; las reglas de
compilación se generan desde ese manifiesto. Sólo las importaciones cubiertas por una
LBS se redirigen. Las restantes mantienen su integración anterior.

## Verificación

```sh
python3 tests/lbs/run_migration.py --accel kvm --settle-ms 2000
python3 tests/lbs/run_migration.py --accel kvm --memory 64 --settle-ms 2000
python3 tests/lbs/run_migration.py --accel kvm --virgl
python3 tests/lbs/run_qemu.py --base build/bleskernos-lbs-user.img --timeout 300
python3 tests/desktop/run_medium_ui_smoke.py --lbs --accel kvm --base build/bleskernos-lbs-user.img --out tests/lbs/evidence/migration/functional-ui
```

La prueba de migración verifica el contenido instalado, la ausencia de copias
en el directorio antiguo y los marcadores de resolución dinámica, espera el intervalo configurado por cliente (4 s por defecto, 2 s en la
repetición final con KVM) y comprueba que sigue ejecutándose; después solicita la salida del proceso completo, recoge su finalización y
comprueba que no quedan hilos activos. Conserva capturas para revisión visual. Omite el lanzador
Wine y su CPL, porque reenvían a otro proceso y terminan, en lugar de mantener
una ventana propia. La regresión funcional existente verifica edición, portapapeles,
guardado y paneles; no se confunde arranque con cobertura completa de cada app.

Los JSON/logs/capturas quedan en `tests/lbs/evidence/migration/`.
La primera entrega y sus limitaciones siguen documentadas en
[LBS-REPORT.md](LBS-REPORT.md); sus cifras corresponden a los binarios anteriores.
Las nuevas exportaciones y el bootstrap incrementan tamaños de algunos clientes.
Las cifras vigentes por aplicación están en `migration/clients.json`.


## Resultados de esta migración

* Auditoría: 27 clientes, exportaciones/importaciones y rutas cdecl correctas;
  13 módulos ELF32 y el cargador host con fallos inyectados, PASS.
* Integración en std/TCG: 270 comprobaciones, PASS. VirGL/TCG con la prueba
  adicional del alias de directorio: 274 comprobaciones, PASS; Mesa y TinyGL
  enviaron dos triángulos a GPU sin fallback en esas escenas.
* Pantalla: previsualizaciones animadas Pipes, Balls y Logo, PASS. El cliente
  obtiene GL/OSMesa de NGS3D.LBS, sin Mesa estático en su imagen.
* Interfaz con KVM/256 MiB: cortar, copiar, pegar, seleccionar, borrar, guardar,
  persistir formato horario de 12 h e idioma ES, PASS; 19 capturas.
* WINE.BEX → Notepad y WINE.CPL → WINE.BEX → winecfg, PASS; se verificaron
  los marcadores SYSTEM.LBS de los forwarders. No se cambió el renderer Wine.
* Los ensayos de UI automáticos TCG fueron sensibles al momento de inyectar
  clics y a la liberación de modificadores; la misma prueba inicial también
  falló sin LBS. Se reforzó la sincronización y se conserva el resultado KVM
  funcional, sin presentar aquellos intentos como pruebas satisfactorias.

Algunos ensayos se ejecutaron concurrentemente. Los tiempos Wine (87,33 s para
Notepad y 40,77 s para winecfg en estas ejecuciones) no constituyen una comparación
de rendimiento con las mediciones anteriores, que usaban otra ruta de prueba.
No se probó hardware físico.


Archivos principales de esta entrega: `sdk/lbs/applications.json`,
`legacy_exports.json`, `tools/generate_lbs_client.py`, `generate_lbs_app_rules.py`,
`lbs_payload.py`, `mk/lbs-apps.mk`; ajustes al generador de exportaciones,
`mk/lbs.mk`, el cargador, `bleskernos_lbs.h`, empaquetador e instaladores.
Los fuentes específicos de las 27 aplicaciones se preservaron: la migración se
aplica a sus referencias al enlazar los binarios instalados.

Comparación de tamaño de objetos ET_REL, antes/después del enlace migrado:

| Cliente | Original | Migrado |
|---|---:|---:|
| Archivos | 59.164 B | 191.476 B |
| Editor | 21.532 B | 142.696 B |
| Apariencia | 2.303.456 B | 197.996 B |
| Pantalla | 2.303.452 B | 197.996 B |

El bootstrap aumenta el tamaño de programas pequeños. En los CPL 3D se elimina
la copia estática de Mesa, con una reducción importante del binario. El JSON
`clients.json` conserva los tamaños exactos vigentes.
Estas mediciones de tamaño pertenecen a la migración inicial. La actualización
[de memoria compartida](LBS-SHARING.md) añade la comprobación física entre procesos.


La repetición final de ciclo de vida de **25 clientes de escritorio** pasó con
256 y 64 MiB, KVM, intervalo de 2 s, cierre del proceso completo y comprobación
de que no quedan hilos activos. Los otros dos clientes son los forwarders Wine
verificados con ventanas Windows reales. Al quitar PRINT.LBS de una copia,
Editor informó el módulo ausente, no ejecutó su cuerpo y `waitpid` confirmó estado
127. La auditoría final pasó y los hashes de los 27 clientes coinciden con los
binarios de la repetición de ciclo de vida (los dos forwarders también están
incluidos en el inventario de esa imagen, aunque sus ventanas se prueban aparte).
