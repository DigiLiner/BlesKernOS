# Informe de implementación LBS — BlesKernOS 0.10

> Informe histórico de la primera entrega. El alcance ampliado de 140 clientes,
> 15 LBS y 11 binarios extra se documenta en [LBS-TOTAL.md](LBS-TOTAL.md).

> Actualización vigente: [páginas físicas compartidas](LBS-SHARING.md), con
> kernel actualizado, datos privados y prueba de COW. Las afirmaciones de código
> privado y kernel intacto de este informe describen sólo la entrega inicial.
> Actualización posterior: [migración de aplicaciones y CPL](LBS-MIGRATION.md).
> Las LBS ahora se instalan directamente en `/SYSTEM`; el informe inicial
> conserva las mediciones y el alcance de la primera entrega.

Fecha: 2026-10-18. Entrega local; no se modificó el kernel ni se sustituyó la
imagen original. La imagen instalada es `build/bleskernos-lbs-user.img`.

## 1. Estado de las bibliotecas

«Funcional» describe el alcance disponible y probado, no compatibilidad universal
con cualquier dispositivo o formato. Ninguna biblioteca está bloqueada.

| Biblioteca | Estado | Implementación y límite principal |
|---|---|---|
| SYSTEM | Funcional | Memoria, archivos, procesos, hilos, reloj e información mediante API nativa existente; trampolines autorizados, sin nuevas syscalls. |
| VISION | Parcial | Ventanas, widgets, botones, menús, pestañas, texto, selección, scroll, diálogos, iconos, foco y eventos existentes. No inventa nuevos controles especializados ausentes del backend; conserva apariencia e integración del escritorio. |
| SOUND | Parcial | PCM u8 mono, buffers, mezcla saturada y volumen; consulta del backend actual. No añade conversión de tasas ni garantiza latencia o enumeración completa de hardware. |
| NETWORK | Funcional | API IPv4 existente: TCP/UDP, DNS, clientes/servidores, errores, modo no bloqueante e información de interfaz. UDP loopback no está implementado por la pila actual; prueba UDP mediante RTL8139/SLIRP. |
| NGS | Funcional | Superficies, primitivas existentes, clipping, escalado, alfa, daño y presentación a Vision. Ráster CPU; presentación usa backend disponible. La API actual transfiere buffers completos, incluso con daño parcial. |
| NGS3D | Funcional | Mesa y plugin TinyGL: contextos, capacidades, color/profundidad, texturas GL, vertex buffers CPU, triángulos, sync y presentación. Software y VirGL verificados. No promete VBO persistente GPU. TinyGL mantiene un contexto actual por proceso; uso de render serializado. |
| MEDIA | Funcional | Recompila decodificadores existentes del Viewer: BKV, AVI, GIF, MPEG1/MP2; imágenes mediante API existente. Buffers prestados con duración documentada. No agrega códecs. |
| INPUT | Funcional | Eventos Vision, estado de teclas/botones, posición y foco; compatibilidad con mecanismos existentes. Sin gamepads, cuya API pública no existe. |
| FONT | Parcial | Selección de fuentes cargadas, dibujo, medición, tamaño/alineación y caché existente. No ofrece carga arbitraria de archivos de fuentes desde usuario. |
| PRINT | Funcional | Enumeración, BPJ, envío, estado real del spool y cancelación en cola. PSFILE probado. Trabajo ya ejecutándose devuelve BUSY; no se acredita impresión física. |
| CRYPTO | Parcial | SHA224/256 y HMAC-SHA256 BearSSL, borrado sensible, RDRAND cuando está disponible y fachada HTTPS/TLS/certificados existente. El proveedor TLS heredado conserva un fallback temporal de entropía: no se certifica su seguridad sin una fuente adecuada. |
| SECURITY | Parcial | Contexto real PID/Ring 3. Autorizar operaciones o cambiar privilegios devuelve UNSUPPORTED: el kernel no publica un mecanismo general de capacidades aplicable. |

Se producen doce módulos de plataforma y `TINYGL.LBS`, plugin opcional de NGS3D.
No se fuerzan todos como dependencias de cada aplicación. El grafo comprobado
está en `sdk/lbs/dependencies.json`; VISION depende de NGS/FONT/INPUT,
MEDIA de NGS/SOUND y CRYPTO de NETWORK; SYSTEM es la base sin dependencias.

## 2. Archivos de esta entrega

Nuevos:

* `mk/lbs.mk`: compilación PIC, enlaces ELF32/i386, SDK, ejemplos y auditoría.
* `sdk/lbs/`: manifiesto, dependencias, política de exportación, metadatos,
  fachadas y servicios de las doce bibliotecas y plugin TinyGL.
* `sdk/lib/lbs.c`, `sdk/lib/lbs_start.c`: apertura con ABI y arranque declarativo.
* `sdk/include/bleskernos_{lbs,lbs_services,lbs_stats,heap,ngs,ngs3d,input_lbs,sound_lbs,media_lbs,print_lbs}.h`.
* `sdk/examples/lbs_demo.c`, `lbs_startup_demo.c`, `lbs_triangle.c`.
* `tools/generate_lbs.py`, `generate_lbs_host.py`, `generate_lbs_docs.py`,
  `install_lbs.py`.
* `tests/lbs/{run_loader_host.py,build_fixtures.py,audit_artifacts.py,run_qemu.py,app_smoke.c,measure_mesa_stack.py}` y evidencias.
* `docs/native/LBS.md`, `LBS-AUDIT.md`, `LBS-EXPORTS.md`, este informe.

Modificados por esta entrega:

* `GNUmakefile`, `Makefile`, `tools/build_fat32_ata.py`, `sdk/README.md`.
* `sdk/include/{dlfcn.h,elf.h,bleskernos_api.h,bleskernos_print.h}`.
* `sdk/lib/{posix_dlfcn.c,posix.c,print.c}` y `programs/network_status.c`.
* Mesa: `src/context.c`, `src/texobj.c`, `src/tnl/t_context.c`,
  `src/array_cache/ac_context.c`, `src/swrast/{s_context.h,s_context.c,s_triangle.c,s_texture.c,s_aatritemp.h}`.

El árbol ya contenía cambios del usuario en kernel, escritorio, aplicaciones,
TinyX, Wine, gráficos y pruebas. No son parte de esta lista ni se revirtieron.
En particular, el cambio previo de `bkmesa_gpu.c` se preservó.

## 3. APIs, formato y ABI

El catálogo [LBS-EXPORTS.md](LBS-EXPORTS.md) se genera con `nm` sobre los ELF
reales; incluye dependencias y símbolos efectivamente exportados. El contrato
público lo definen las cabeceras del SDK y [LBS.md](LBS.md), con ejemplos de
compilación, enlace, exportación, instalación, carga dinámica y arranque.
Los símbolos internos de Mesa/TinyGL que aparezcan en el catálogo no son
una promesa de ABI pública; deben usarse las cabeceras y `get_proc` documentados.

Son ELF32/i386 ET_DYN con SONAME, SysV hash, DT_NEEDED, REL, init/fini,
metadatos ABI mayor 1 y validación de ABI nativa del sistema. Directorio principal:
`/SYSTEM/LIBS/NATIVE`. Se amplió el `dlopen` Ring 3 existente; el cargador Linux
no cambió. Se preserva la búsqueda SO, incluyendo el directorio actual.

El puente optativo de importaciones resuelve trampolines nativos del ejecutable
ET_REL, nunca direcciones privilegiadas del kernel. Las imágenes LBS son privadas
por proceso; dependencias y referencias se deduplican dentro del proceso.
El arranque declarativo carga requisitos antes del punto de entrada del usuario
y descarga en orden inverso al retornar o ante error.

## 4. Migración y programas

Network Status utiliza NETWORK.LBS para información, DHCP, DNS y ping. Conserva
su interfaz existente y carga únicamente NETWORK y su cierre de dependencias.
La suite confirma su ejecución real mediante el marcador de migración.

`LBSTRI.BEX` es una demo independiente de ventana Vision + triángulo NGS3D,
instalada en `/SYSTEM/PROGRAMS`. `LBSSTART.BEX` verifica el arranque declarativo;
`LBSTEST.BEX` es la integración completa, que requiere fixtures y el servidor
UDP del runner. No se migró masivamente el resto de aplicaciones.

## 5. Pruebas y resultados

Comandos de reproducción desde la raíz:

```sh
make -j4 sdk
make -j4 lbs-audit build/programs/network_status.o
python3 tests/lbs/run_qemu.py --timeout 300
python3 tests/lbs/run_qemu.py --memory 64 --timeout 300
python3 tests/lbs/run_qemu.py --vga vmware --timeout 300
python3 tests/lbs/run_qemu.py --virgl --timeout 300
python3 tests/lbs/run_qemu.py --cpu qemu32,+rdrand --timeout 300
python3 tests/lbs/run_qemu.py --triangle --timeout 180
python3 tests/lbs/measure_mesa_stack.py
python3 tools/generate_lbs_docs.py
python3 tools/install_lbs.py
python3 tests/kernel/run_wine_gui_smoke.py --base build/bleskernos-lbs-user.img --out tests/lbs/evidence/wine-lbs-regression --startup-only --kernel-dir build
```

Resultados guardados en `tests/lbs/evidence/`, con logs, capturas, WAV y hashes
de entrada por ejecución:

| Configuración | Resultado | Alcance |
|---|---|---|
| TCG std, 256 MiB, 2 CPU | PASS | Integración completa; última ejecución aislada con medición 2D adicional. |
| TCG std, 64 MiB | PASS, 269 comprobaciones | Memoria limitada, mismo conjunto funcional. |
| TCG VMware, 256 MiB | PASS, 269 comprobaciones | Presentación y fallback software funcional. |
| TCG VirtIO/VirGL, 256 MiB | PASS, 271 comprobaciones | Mesa AUTO y TinyGL AUTO: 2 draws/2 triángulos en GPU, cero fallbacks en esas escenas, lectura de píxeles/texturas verificada. |
| TCG std con RDRAND emulado | PASS | Fuente aleatoria disponible declarada; no es validación de hardware físico. |
| Demo LBSTRI | PASS | Ventana, escena y liberación sin fixtures usados por la demo. |
| Wine Notepad | PASS | Primera superficie en 26.926 ms sobre imagen instalada; 512 MiB, KVM, 2 CPU. Resultado separado del ensayo de la imagen original. |

Los cuatro ensayos de matriz se ejecutaron concurrentemente. Posteriormente se
repitió std/256 de forma aislada. `inputs.json` permite identificar exactamente
qué binarios se usaron; la última demo añade una medición 2D, sin cambiar las
bibliotecas. La última corrección del error cuando falla la creación de TLS del
cargador se verificó en host y std; la matriz anterior verificó la ruta normal.

El test host ejecuta el cargador i386 real: símbolos, dependencias, constructores,
destructores, referencias, LOCAL/GLOBAL, proveedores globales retenidos,
ciclos, handles inválidos, 13 mutaciones ELF, 25 posiciones de fallo de asignación,
rollback, recursos vivos, DAG diamante de 24 módulos, 400 aperturas/cierres desde
hilos y separación de errores TLS, incluyendo fallo al crear la clave.
La auditoría valida los 13 ET_DYN, ABI, imports, exports y DAG real.

QEMU verifica llamadas reales a las doce bibliotecas, dos procesos simultáneos,
audio PCM AC97 con WAV no silencioso, mezcla saturada, TCP loopback y UDP por
NIC con eco en el host, imagen BMP y video GIF, entrada de teclado/mouse por QMP,
fuente nativa, alfa/escalado/clipping 2D y presentación, Mesa/TinyGL software y
GPU con lectura de píxeles, texturas y AA/multitextura software. PRINT genera
PostScript y comprueba estado terminado; otro trabajo en cola se cancela.
SECURITY devuelve UNSUPPORTED donde corresponde. Hash/HMAC contrastan vectores
conocidos; la fuente RNG ausente se informa correctamente.

Regresiones: POSIX nativo carga DT_NEEDED SO y completa su smoke; SDL2 demo
completa; Filemanager, Editor, Paint, Imageviewer, Midamp y Viewer se ejecutan y
se limpian. Son pruebas de arranque y ciclo de vida, no cobertura exhaustiva de
cada operación de sus interfaces. Wine prueba primera superficie de Notepad,
no toda su funcionalidad. En VirGL/EGL no hay screendump CPU disponible; se
conservan las comprobaciones de píxeles del backend y la nota del runner.

También pasaron `tests/sdk/test_sdk_libc.py`,
`tests/kernel/test_native_api_abi.py`, `audit_native_api_boundary.py`,
`test_elf_object_bounds.py` y `tests/desktop/test_mesa_gpu_state.py`.
Compilación completa de archivos SDK, comprobación de Python y `git diff --check`
sobre los archivos de esta entrega. No se probó hardware físico.

## 6. Mediciones y memoria

`std-256m/result.json` conserva las mediciones vigentes. En la última ejecución aislada
(270 comprobaciones, 56,45 s): cargar las doce bibliotecas tomó 2260 ms;
100 aperturas/cierres con referencias calientes, 20 ms; cinco ciclos fríos con
contexto Mesa, 2030 ms. Cien fills de 400×250 tomaron 540 ms y una presentación
10 ms (resolución del reloj del invitado). Es TCG: no representa latencia nativa de hardware.
Los campos `ngs_fill_100_ms` (100 fills de 400×250) y `ngs_present_ms` (una
presentación) separan trabajo 2D y envío del frame; no equivalen a tiempo hasta
scanout. `frame_ms` mide la secuencia 3D de prueba, no un benchmark de FPS.
No existe un baseline gráfico equivalente anterior, por lo que no se afirma
una mejora porcentual ni ausencia absoluta de regresiones de rendimiento.

Las doce imágenes suman 1.769.472 bytes privados residentes asignados por el
cargador. El proceso de prueba ocupa aproximadamente 2,12 MB al cargarlas.
Tras cinco ciclos fríos: cero asignaciones vivas antes/después, cero diferencia
de memoria del proceso. El allocator conserva un arena de 262.207 bytes, igual
antes/después; no se confunde esa reserva con una fuga.

Network Status pasa de 8.900 a 102.980 bytes ET_REL (+94.080), medido con las
mismas opciones de compilación sobre su fuente original de HEAD y la migrada.
El bootstrap/cargador/POSIX estático supera el ahorro de esta aplicación pequeña.
La migración no acredita reducción de tamaño. Evidencia: `executable-size.json`.

Las páginas físicas de código **no se comparten entre procesos**. Para hacerlo
bien faltan mappings de archivo RO compartidos, GOT/datos privados y una política
segura de relocaciones. Las referencias actuales no sustituyen esos mecanismos.

## 7. Problemas corregidos

* Validación incompleta de ELF, strings, límites, REL y callbacks de init/fini;
  rechazo explícito de TLS tardío y RELA no soportados.
* Handles inválidos, dependencias circulares, ámbitos de símbolos, proveedores
  descargados antes de sus consumidores y rollback de cargas fallidas.
* Resolución exponencial de símbolos en DAG con diamantes; ahora recorrido BFS
  con visitados. Bloqueo recursivo por hilo y errores independientes mediante TLS.
* Verificación de descriptor propio: una dependencia no puede suplir la ABI del
  módulo que se está abriendo. Descarga rechazada con recursos vivos del cierre
  transitivo, antes de modificar referencias.
* Fugas heredadas de Mesa: caché de immediate TNL, matrices derivadas, contexto
  array-cache, proxy cube map y caras adicionales de texturas; doble liberación
  potencial al fallar parcialmente la creación del proxy.
* Rasterizador Mesa usaba hasta 304.112 bytes de stack, incompatible con stacks
  habituales de 64 KiB. Workspaces diferidos por contexto, reutilizados y liberados;
  máximo compilado de los cuatro fuentes inspeccionados: 25.024 bytes. No es una
  prueba universal de profundidad máxima de toda la pila de llamadas.
* PRINT calculaba identificadores TMP/BPJ con ticks distintos; ahora identidad
  estable por trabajo y secuencia atómica, sin cambiar el formato BPJ.
* Presentación NGS/Mesa/TinyGL activa streaming cuando corresponde y reutiliza
  el modo; evita pedir el cambio en cada frame.
* El test TinyGL comenzaba otro frame sin clear, provocando fallback legítimo;
  ahora respeta el comienzo del frame. Diferencia de un LSB en textura azul de
  TinyGL documentada y acotada en el test.

## 8. Limitaciones pendientes

Además de los límites por biblioteca: imágenes de código privadas; sin W^X/RELRO
nuevo en el cargador SDK; sin TLS dinámico tardío, RELA o versionado GNU de
símbolos. No se cambió la protección de páginas ni el cargador Linux/Wine.
Se mantiene el ABI mayor explícito de LBS y se rechaza la incompatibilidad.

El usuario debe respetar propiedad y duración de callbacks, buffers y recursos.
Los recursos creados por las fachadas nuevas se contabilizan para impedir una
descarga prematura; las APIs nativas heredadas conservan su contrato de cierre
por el consumidor. El pin explícito dura toda la vida del proceso.
No hay impresión física, audio físico, prueba prolongada de GPU o auditoría
criptográfica formal. HTTPS reutiliza el TLS existente; no se validó contra un
servicio público desde la suite.

## 9. Riesgos de regresión

El cargador POSIX endurecido rechaza ELF malformados que antes podía aceptar y
cambia el ámbito LOCAL/GLOBAL al contrato documentado. SO existentes de prueba
funcionan; no se garantiza cubrir todas las SO externas posibles.
Los arreglos Mesa afectan también a la biblioteca estática nativa: compilación,
pruebas de estado y escenas reales pasaron, pero falta estrés prolongado.
El bootstrap aumenta el tamaño y coste inicial de programas pequeños.
No se repaquetó toda la distribución con `make all`: se integraron reglas y se
actualizó una copia offline de la imagen existente para preservar datos locales.

## 10. Próximas mejoras

1. Compartir mappings de código inmutables y reducir el bootstrap por aplicación;
   medir nuevamente tamaño y latencias con baseline reproducible en hardware.
2. Subida parcial de superficies y buffers GPU persistentes donde el driver
   publique soporte; estrés de contextos, ventanas, texturas y pérdidas de GPU.
3. Fuente de entropía del kernel y TLS que falle de forma cerrada cuando falte;
   autorización real aplicada por kernel antes de ampliar SECURITY.
4. Ampliar audio, fuentes y controles exclusivamente con backends reales, más
   pruebas funcionales de las aplicaciones y dispositivos físicos.
5. Restringir aún más exportaciones internas GL a una lista de símbolos públicos,
   conservando ABI documentada; ampliar fuzzing del cargador y fallos de recursos.
