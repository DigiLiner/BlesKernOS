# Parches a Wine

**Ninguno aplicado.** La fuente Wine 11.0 descargada coincide con el SHA256 fijado
en `versions/wine-11.0.json` y se extrae fuera del kernel. Los cambios de esta
serie pertenecen al host BlesK, al interprete didactico o a las pruebas.

No se aplicaron los parches de BoxedWine por analogia. Antes de agregar un parche
se debe registrar aqui archivo upstream, expectativa del host, diferencia BlesK,
prueba que lo exige y condicion para retirarlo.

Configure fue ejecutado desde la fuente fijada. Decisiones:

| Decision | Motivo |
|---|---|
| GCC/G++ -m32, ld elf_i386 | Runtime Unix puramente i386 |
| --without-mingw | Estrategia Unix de la referencia BoxedWine; opcion comprobada |
| --with-x, --with-freetype, --with-fontconfig | Requisitos graficos/fuentes explicitos; configure debe fallar si faltan |
| --disable-tests | Reduce primer build; no elimina tests/win32 ni la escalera de validacion |
| Sin Wayland/Vulkan/GStreamer/FFmpeg | Fuera del bootstrap Office 95 |
| Sin udev/USB/v4l2/SANE/gphoto | Hardware opcional no requerido por la primera aplicacion |
| Sin CUPS/Pulse/DBus | Impresion/audio/integracion de escritorio pospuestos |
| Sin XShm/XComposite | Primer puente sin memoria X compartida ni composicion X externa |
| Sin cambios Win16/COM/OLE/registro | No recortar componentes de Windows requeridos por instaladores |

Las dependencias restantes conservan el comportamiento de deteccion upstream;
el resultado exacto esta en configure.log/config.log, conservados en el paquete.
Build/install completos. Se agregaron wrappers locales as --32 y ld -m elf_i386 para que Winebuild genere tambien las import libraries i386. No modifican fuentes upstream.
El empaquetado registra cambios PT_INTERP/RUNPATH y hashes antes/despues. Glibc y ld.so se conservan byte por byte; sus hashes se verifican al empaquetar.

