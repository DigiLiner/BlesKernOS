# Plan de migración de código a LBS

Fecha de revisión: 2026-10-20. Este documento conserva debajo la auditoría
inicial para explicar el alcance. La implementación posterior se describe en
[LBS-MIGRATION.md](LBS-MIGRATION.md) y sus pruebas en
[LBS-TOTAL.md](LBS-TOTAL.md). Los conteos y carencias de la auditoría inicial
no describen los artefactos finales.

## Implementación posterior a la auditoría

El manifiesto ahora tiene 140 clientes y la plataforma 15 módulos. Se extrajeron
los decodificadores del Viewer, widgets, raster de Imageviewer, archivos BKZ1 y
ZIP/DEFLATE, y helpers de comandos. Se migraron los clientes nativos pendientes,
comandos y salvapantallas. El selector de payloads y el SDK externo utilizan
los mismos contratos. Los servicios y recuperación tienen excepciones explícitas.

Las fuentes de los extras se recuperaron de la ruta local proporcionada por el
usuario, preservando Codex 2.6.1 y la versión ampliada de 3D Plus. El inventario
local completo está en `tests/lbs/evidence/total/local-extras-inventory.json`.
Las pruebas finales y límites se mantienen en LBS-TOTAL.md.

## Auditoría inicial (histórica)

## Objetivo

Que las aplicaciones nativas obtengan los servicios comunes de bibliotecas
versionadas, y que los algoritmos reutilizables tengan una implementación
canónica. Conservar la funcionalidad, los límites de privilegio y los contratos
de recursos. Cada programa carga sólo los módulos que necesita.

Dos trabajos distintos: redirigir importaciones existentes hacia LBS y extraer
implementaciones locales. El primero ya está avanzado; no acredita por sí solo
el segundo. No se fija un objetivo de reducir todo ejecutable ni de mejorar FPS:
el bootstrap tiene coste, y compartir código no acelera automáticamente el render.

## Inventario observado

### Clientes nativos

`sdk/lbs/applications.json` declara 27 clientes: Archivos, Editor, Calculadora,
Fuentes, Midamp, Procesos, Calendario, Acerca de, Ejecutar, Imageviewer, Viewer,
Paint, ScanDisk, lanzador Wine, Panel de control y doce CPL. Los objetos migrados
están en `build/lbs/apps/`; los objetos originales siguen existiendo.

Proveedores directos en los informes de esos 27 objetos (no incluyen dependencias
transitivas ni el cliente independiente Network Status): SYSTEM 27; VISION,
NGS y FONT 25 cada uno; INPUT 22; MEDIA 8; SOUND 6; NGS3D 2; PRINT 1; NETWORK 1.
No aparecen CRYPTO/SECURITY como proveedores directos en ese conjunto. No hace
falta forzar esas dependencias en programas que no las necesitan.

Network Status carga NETWORK explícitamente para cuatro operaciones; su interfaz
conserva llamadas nativas. Es adopción parcial por subsistema.

Fuera del manifiesto: NetSurf, Shell, Help Center, Find Files, Archive Manager,
Disk Tools, Clipboard Viewer, Defrag, Event Viewer y Performance Monitor.
Los comandos de consola, salvapantallas independientes y utilidades especiales
de arranque/instalación también necesitan clasificación propia. Gears y las
pruebas de la edición developer no están migrados por ese manifiesto.

Los clientes migrados conservan lógica local: Viewer enlaza los decodificadores
que también se compilan en MEDIA.LBS. La API MEDIA actual necesita ampliarse
antes de sustituir todas sus llamadas vv_*: metadatos, tiempos por frame,
consulta de audio y búsqueda/sincronización de audio son ejemplos a cubrir.

### Imágenes

En la inspección de esta sesión:

- `build/bleskernos-ata-user.img`: las 13 LBS coinciden con las compiladas;
  ninguno de los 27 clientes coincide con el objeto migrado y ninguno presenta
  sus marcadores de dispatch dinámico. Network Status sí coincide con su objeto
  actual y contiene el marcador de migración.
- `build/bleskernos-lbs-user.img`: los 27 clientes coinciden con los objetos
  migrados, pero falta `/SYSTEM/VISION.LBS`. Las otras doce LBS coinciden.

Las reglas de empaquetado actuales seleccionan los clientes migrados; la
configuración de build y el contenido efectivo de una imagen son evidencias
distintas. No se comprobó en esta revisión el contenido de USB/VMDK.

### OS y servicios

VISION.BEX es el servidor de escritorio y utiliza una ABI privada de servicio.
VISION.LBS es una biblioteca de cliente. No son intercambiables. Escritorio,
compositor y servicios no están incluidos en la migración de los 27 clientes.

Mantener en kernel/drivers el acceso a hardware, VM, planificación, comprobación
de punteros y autorización. Mantener en los servicios la propiedad y arbitraje
de ventanas, dispositivos y trabajos. Las LBS pueden contener operaciones CPU
reutilizables y clientes de esos servicios. Evitar que un servidor cargue una
biblioteca que reenvíe solicitudes hacia él mismo.

No exigir dependencias dinámicas a recuperación o arranque temprano sin decidir
primero su política de disponibilidad. Pueden compilar una variante estática
desde la misma fuente canónica y documentar esa excepción.

### Extras

| Extra | Evidencia y alcance |
|---|---|
| Quake II nativo | `platform/glimp_blesk.c` abre NGS3D.LBS y resuelve contextos, presentación y GL. El binario contiene el cargador. SDL2/POSIX y lógica del juego siguen enlazados localmente. Su ruta software no usa NGS3D para rasterizar. |
| Instalador Quake II | Makefile y binario: SDK estático, sin bk_lbs_open. Clasificar como instalador, no como cliente ya migrado. |
| Bles Codex | Referenciado por Makefile y herramientas Codex Pro; falta `extras/Bles-Codex32-standalone`. Estado del ejecutable no verificable aquí. |
| HyperZip | Falta `extras/HyperZip-BlesKernOS-Standalone`; estado no verificable. |
| LexiNet | Falta `extras/LexiNet`; estado no verificable. |
| Civ2 | Falta `extras/Civ2-BlesKernOS-Standalone`; estado no verificable. |

El empaquetador extra copia cuatro objetos y no aplica `migrated_source`.
Eso no demuestra cómo fueron enlazados en sus repositorios externos. También
falta actualmente `tests/fixtures/disk-images/programas-extra.img`.
Las plantillas actuales de Codex Pro enlazan SDK estático; añadir una plantilla
LBS y su puente es un trabajo independiente de migrar el IDE.

Los ejecutables Windows usan Wine y su ABI Linux/Windows: no cargan directamente
las LBS nativas. Mantener esa frontera; un adaptador futuro sería otro proyecto.

## Destino del código

| Módulo | Responsabilidad propuesta | Primeros consumidores |
|---|---|---|
| SYSTEM | API de archivos, procesos, reloj y memoria; helpers comunes con suficiente entidad. Sin sustituir kernel ni ampliar indiscriminadamente sus exports. | Utilidades pendientes, Shell y extras auditados. |
| VISION | Fachada de ventanas/controles; diálogos y helpers de interfaz reutilizables que operen con handles públicos. El servidor conserva su autoridad y estado global. | Todas las GUI nativas, NetSurf y extras compatibles. |
| NGS | Superficies CPU, clipping, escalado, alfa y presentación. Separar procesamiento de píxeles de la política de cada aplicación. | Imageviewer, Paint, Viewer. |
| FONT | Medición y dibujo de texto, selección y acceso al servicio/caché existente. No duplicar cachés ni mover el estado global al cliente. | GUI nativas y NetSurf mediante adaptador. |
| INPUT | Estado por ventana y adaptación de eventos/foco; conservar la política del servicio. | GUI, juegos y salvapantallas. |
| MEDIA | Decodificadores, metadatos, frames, tiempos y seek/audio. Fuente canónica fuera del directorio privado de Viewer. | Viewer primero, Imageviewer y NetSurf después. |
| SOUND | Cliente de audio y mezcla existente; conversión/remuestreo sólo como ampliación posterior separada. Hardware y arbitraje siguen en el backend. | Midamp, Viewer, juegos. |
| NGS3D / TINYGL | Contextos y backends 3D, GL/OSMesa de compatibilidad. Conservar presentación GPU directa. | CPL 3D, Quake II; después salvapantallas y Gears. |
| PRINT | Generación de trabajos y cliente del spool. El spooler mantiene ejecución y estado. | Editor y futuros consumidores de impresión. |
| NETWORK / CRYPTO | Separar transporte/protocolos compartidos de las operaciones criptográficas. Inventariar primero lo alojado en servicios/kernel. | Network Status, NetSurf y extras con uso de red confirmado. |
| SECURITY | Consultar contexto y capacidades realmente disponibles. La biblioteca no puede crear autorización que el kernel no aplica. | Sólo clientes que necesiten estas consultas. |
| ARCHIVE, nueva | BKZ1: inspección, límites y compresión/extracción. Se encontró implementación duplicada en Archive Manager y comandos compress/extract. | Esos tres clientes; HyperZip sólo tras verificar formatos y fuente. |

SDL2.LBS queda como candidata posterior: hoy SDL2 se enlaza estáticamente y
requiere estudiar ABI, callbacks, estado y carga. No es requisito del primer
ciclo. No se proponen bibliotecas específicas de cada aplicación ni exportar
todo el interior de NetSurf, el Editor o un juego.

## Orden de ejecución y aceptación

1. **Distribución coherente.** Generar/actualizar una copia de la imagen,
   instalar VISION.LBS y los 27 clientes actuales; comparar hashes. Verificar
   arranque, apertura/cierre y pruebas funcionales. Validar después los derivados
   USB/VMDK. No sobrescribir discos activos ni sacrificar datos del usuario.
2. **Contrato y build.** Mantener headers públicos, ABI mayor y ampliaciones
   compatibles; DTO con tamaño/versionado y objetos opacos. Definir ownership,
   errores, callbacks, threading y descarga. Unificar la selección de payloads
   para imágenes y extras. Identificar proveedor por símbolo en cada cliente.
3. **Piloto MEDIA/Viewer.** Completar primero las operaciones que usa Viewer,
   conservar formatos, timeline, audio y seek. Mover la fuente canónica de codecs
   a un módulo compartido; quitar la copia enlazada al cliente. Comparar símbolos,
   resultados y memoria. No cambiar codecs ni añadir formatos en esta etapa.
4. **Completar clientes GUI.** Migrar las utilidades pendientes por grupos y
   completar la GUI de Network Status. Adaptar NetSurf con sus backends de dibujo,
   texto, imágenes y red, conservando inicialmente su motor HTML/CSS privado.
5. **Extracciones concretas.** ARCHIVE para BKZ1; operaciones de imagen comunes
   hacia NGS/MEDIA y diálogos/helpers hacia VISION. Migrar audio reutilizable
   hacia SOUND. Conservar undo, herramientas, políticas y documentos en las apps.
6. **3D y servicios.** Salvapantallas/Gears hacia NGS3D; medir FPS, transferencias
   y CPU. Clasificar dependencias de escritorio/servicios por separado; extraer
   sólo algoritmos sin dependencia circular y mantener ABI privada separada.
7. **Extras y SDK.** Auditar los cuatro repositorios ausentes cuando estén
   disponibles, registrar qué código aportan y migrar sólo lo común. Mantener
   Quake II como regresión de NGS3D. Añadir ejemplo y plantilla LBS a Codex Pro,
   además de una política explícita para instaladores y recuperación.
8. **Cierre.** Auditar toda edición e imagen, reducir exportaciones privadas y
   documentar excepciones. Conservar compatibilidad legacy mientras existan
   consumidores; retirar cada copia estática sólo tras comprobar su reemplazo.

## Resultado final comprobable

- Inventario de cada aplicación, extra y servicio: módulos directos/transitivos,
  símbolos, código local deliberado y excepciones de arranque/recuperación.
- Todas las aplicaciones GUI nativas en alcance usan los proveedores acordados;
  archivos instalados y binarios auditados son exactamente los mismos.
- Una implementación canónica por algoritmo extraído. Los consumidores ya no
  enlazan sus copias locales; un módulo cargado sin uso no cuenta como migración.
- Actualización compatible de una LBS beneficia a un cliente ya compilado.
  Cambios incompatibles exigen nueva ABI y no sustitución silenciosa.
- Recursos, buffers, callbacks e hilos conservan su ciclo de vida. Fallos de carga
  son explícitos; rollback y cierre no dejan recursos ni tareas activas.
- Regresión funcional: archivos/edición/guardado, formatos y seek/audio,
  impresión, red, controles, salvapantallas y Quake II. Probar módulos ausentes,
  ABI incorrecta y datos malformados según el componente migrado.
- Medir antes/después: bytes de ejecutables y distribución completa, memoria
  privada y páginas físicas compartidas, arranque frío/caliente, CPU, audio y
  presentación/FPS. Contar mappings virtuales no acredita ahorro de RAM física.
- No se promete una mejora de rendimiento sólo por convertir código a LBS;
  aceptar regresiones únicamente con causa, medición y decisión explícitas.

## Evidencia disponible de la revisión

Pasaron en esta sesión las auditorías `tests/lbs/audit_clients.py` (27),
`audit_artifacts.py` (13) y `run_loader_host.py` (ELF inválido, rollback,
mappings y concurrencia). Quake II se inspeccionó en fuentes, reglas y símbolos
de los binarios existentes. No se ejecutó de nuevo la matriz QEMU, ni se probó
hardware físico, ni se evaluaron los cuatro extras ausentes.
