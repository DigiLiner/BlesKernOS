# Migración ampliada de bibliotecas nativas

Alcance de la entrega y evidencias verificables. Las pruebas fallidas se
conservan como diagnóstico y no se cuentan como aprobadas. Las imágenes se
verifican contra los payloads construidos y conservan los datos del usuario.

## Alcance

El manifiesto `sdk/lbs/applications.json` contiene 140 clientes: 36 aplicaciones
gráficas y CPL, 98 comandos, tres salvapantallas, dos lanzadores Wine y Gears
para developer. La edición user instala 139 de esos clientes. Los ejecutables
instalables están en `build/lbs/apps`; los objetos originales son intermedios.

Las 15 LBS se instalan directamente en `/SYSTEM`: SYSTEM, INPUT, FONT, NGS,
SOUND, NETWORK, VISION, NGS3D, MEDIA, PRINT, CRYPTO, SECURITY, TINYGL, ARCHIVE
y COMMAND. [LBS-MIGRATION.md](LBS-MIGRATION.md) describe las extracciones.
El catálogo [LBS-EXPORTS.md](LBS-EXPORTS.md) procede de los ELF construidos.

MEDIA contiene los decodificadores canónicos y Viewer usa handles opacos,
metadatos, seek, streaming AVI/MPEG y audio. VISION contiene los widgets comunes;
NGS contiene el raster escalado y la transparencia. ARCHIVE contiene BKZ1 y el
ZIP/DEFLATE/CRC de HyperZip. COMMAND contiene los helpers compartidos de los
comandos. Mesa y TinyGL se suministran mediante sus bibliotecas de renderizado.

El SDK exporta contratos y runtime para aplicaciones externas. Los archivos
`LIBLBSPX.A` y `LIBLBSNT.A` aíslan los descriptores POSIX de los handles VFS;
`LIBLBS.A` y `LBSHOST.O` proporcionan arranque e importaciones del cliente.
Los JSON junto a cada ejecutable registran proveedores e importaciones. El
empaquetador selecciona los bytes migrados antes de preparar el payload, tanto
para instalaciones normales como para actualizaciones.

## Extras y procedencia

Hay 11 binarios nativos en `sdk/lbs/extra_applications.json`: Codex, HyperZip,
LexiNet, el lanzador 3D Plus, sus variantes Mesa y TinyGL, Doom, Quake, el
cliente y servidor Freeciv, y Quake II. Se consultó inicialmente
BlesKernOS-Programs y después se incorporaron las fuentes locales del usuario.
Codex 2.6.1, 3D Plus y Bles3D-Core se tomaron de esa fuente local. Freeciv 3.2.5
conserva su nombre y sus procesos separados; no se renombra como Civ2.

Freeciv conserva su ABI privada de FILE, sockets y asignación, con símbolos
`fc_private_*` para evitar interponerla sobre el runtime del cargador. Sus
llamadas nativas sí pasan por LBS. Los píxeles destinados a la API de imágenes
se asignan en el dominio nativo correspondiente. El cliente puede cancelar la
carga y solicita la salida exclusivamente al servidor local que él inició.
Quake II atiende la salida también durante el registro de recursos. NGS3D es
opcional en su arranque con software y queda disponible para el renderer GL.

`tests/lbs/evidence/total/local-extras-inventory.json` enumera también fuentes
originales y otros ports históricos. Ese inventario no acredita migración de
esos otros ports, que quedan fuera del manifiesto de esta entrega. Las fuentes
locales originales no se modificaron.

El disco suplementario `tests/fixtures/disk-images/programas-extra.img` contiene
los 11 ejecutables, las 15 LBS y los datos disponibles de los juegos. Se
comprobaron sus bytes contra los construidos en `extras-installed.json`.

## Comprobaciones aprobadas

Todas las rutas siguientes son relativas al repositorio:

| Comprobación | Evidencia |
| --- | --- |
| 15 ELF32 ET_DYN, ABI, dependencias acíclicas e importaciones | `build/lbs/total-final-artifact-audit.log` |
| 140 clientes, llamadas redirigidas y ausencia de implementaciones extraídas | `build/lbs/total-final-client-audit.log` |
| Apertura y cierre de los 36 GUI con 64 y 256 MB | `tests/lbs/evidence/migration/std-{64,256}m-kvm-final/result.json` |
| Integración: 316 comprobaciones con 256 MB; 308 con 64 MB | `tests/lbs/evidence/std-{64,256}m-kvm/result.json` |
| Compartición física, COW, protección de código y liberación del último usuario | `tests/lbs/evidence/sharing/kvm-64m/result.json` |
| Edición, guardado, portapapeles, configuración y ventanas; 19 capturas revisadas | `tests/lbs/evidence/total/ui/result.json` |
| Seis hashes de imagen iguales al baseline y zoom/pan/ajuste | `tests/lbs/evidence/total/imageviewer/after/result.json` |
| Paint: dibujo, relleno, borrado, continuidad y BMP guardado | `tests/lbs/evidence/total/paint/after/result.json` |
| Logo, Balls y Pipes: animación y cierre | `tests/lbs/evidence/total/savers/after/*/result.json` |
| Compress, Extract, Copy, Echo y Checksum; round trip exacto | `tests/lbs/evidence/total/commands/result.json` |
| Cuatro cierres de NetSurf con descargas activas | `tests/lbs/evidence/migration/std-256m-kvm-settle2000-ns-fixed/result.json` |
| Wine: Notepad mediante WINE.BEX y winecfg mediante WINE.CPL | `tests/lbs/evidence/total/wine-{program,cpl}-final/result.json` |
| Quake II OpenGL: VirGL activo, cero fallbacks y cero downloads | `extras/quake2/evidence-perf/lbs-total/result.json` |
| Quake II: cierre durante la carga | `tests/lbs/evidence/migration/std-512m-kvm-settle8000-clock-exit-fixed-extras/result.json` |
| 11 extras: importaciones públicas, ZIP y 3D compartidos | `tests/lbs/evidence/total/extras.json` |
| Freeciv: entradas originales, imports y relocaciones de cliente y servidor | `build/lbs/total-freeciv-port-check.log` |

También se comprobaron BKZ1 con ASan, el raster con recortes y guardas, streaming
AVI/MPEG y audio MP2, y el cargador i386 con entradas malformadas y fallos de
asignación/mapeo. Las pruebas del host complementan las ejecuciones en el OS.

Se corrigió la propiedad de las asignaciones persistentes de las cachés de
fuentes y TLS: no pueden pertenecer al programa que pidió el primer recurso,
porque el OS las reutiliza después de que ese proceso termine. El kernel, el
servicio Vision y TLS.DVR se incluyen juntos en la imagen de validación.

## Tamaño y resultados esperados

Las 15 LBS ocupan 2.144.228 bytes. Los 140 clientes ocupan 17.642.576 bytes.
Para los 125 destinos con baseline comparable, los ejecutables pasan de
14.819.088 a 15.988.368 bytes. El bootstrap dinámico aumenta varios clientes
pequeños; otros se reducen al dejar de incluir motores privados. No se afirma
que el tamaño de la distribución completa disminuya.

La prueba con dos procesos y CR3 distintos verifica 374 páginas de código y
rodata físicamente compartidas: 1.531.904 bytes de ahorro, con datos privados
y COW. El resultado es una implementación común que se actualiza y verifica en
un lugar, contratos disponibles para programas externos y páginas de código
compartidas. La migración no implica por sí sola más FPS.

## Límites arquitectónicos

Kernel, drivers y servidores conservan su autoridad; los servicios no deben
usar wrappers que los hagan llamarse a sí mismos por IPC. Los seis comandos de
recuperación siguen autocontenidos: bosrecovery, fdisk, format, diskpart, mount
y unmount, reutilizando fuentes canónicas. El instalador Quake II conserva su
independencia para reparar sistemas anteriores. Wine/Linux mantienen sus ABI.
SDL2 sigue enlazado en los ports que lo requieren; SDL2.LBS queda pendiente de
una extracción separada. La lógica específica de cada programa permanece en él.

Las pruebas se ejecutan en QEMU/KVM, no en hardware físico. Las pruebas de
lifecycle de juegos acreditan arranque y cierre, no una partida completa ni el
funcionamiento de cada función del port histórico.

## Imágenes

`build/bleskernos-lbs-user.img` es la instalación de referencia. ATA y USB user
contienen la misma partición; VMware utiliza una partición BFS4 ampliada a
4095 MiB dentro de un VMDK de 4096 MiB. Los bytes de los ejecutables, LBS, SDK y
documentación se cotejan con sus fuentes de instalación. La actualización
conserva stage 1 y los archivos de configuración y registro Wine; instala el
kernel y stage 2 correspondientes a la ABI compartida. La imagen anterior se
conserva como `build/bleskernos-ata-user.pre-lbs-total.img`.

La comprobación 3D ejecuta 20 frames del mapa base1 con VirtIO/VirGL; verifica
GPU activa, presentación GPU y ausencia de fallbacks o descargas. Es una prueba
de la ruta acelerada tras migrar, no una comparación de FPS con el sistema
anterior. El primer intento de Wine falló al crear la ventana; la repetición
con los dos lanzadores migrados y el control directo abrieron sus ventanas.
No se certifica una ejecución completa de todos los programas de Windows.
