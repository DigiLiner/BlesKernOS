# Ampliación por familias — implementación y validación

Imagen entregada: `build/bleskernos-family-drivers.img` (768 MiB lógicos).
Cobertura, código y límites: [family-drivers.md](platform/family-drivers.md).

Se añaden OHCI para HID/printer/CDC-ECM de puertos raíz, clases bulk sobre EHCI,
PRO/100 (23 IDs PCI), Ethernet USB CDC-ECM, detección de lenguajes de impresora y
933 identidades gráficas exactas con framebuffer de firmware condicionado.
El catálogo tiene 407 ATI, 392 NVIDIA y 134 de Cirrus/S3/Matrox/3dfx/SiS/Trident.
**La cifra es de candidatos por identidad, no de 933 placas probadas físicamente.**

## Resultado

- **22 ejecuciones host: PASS**. Límites de los ocho motores Ethernet anteriores
  y cuatro PCM; parser/capacidades CDC-ECM y TX ZLP; selección de lenguaje
  IEEE1284; raster/render/JBIG ZJS; firmware HP y longitudes USB; catálogo completo
  y aperturas/pitch; finalización EHCI; TD/ED OHCI y cuarentena DMA; PS/2 opcional;
  confirmación tardía de servicios con protección frente a PID muerto/reemplazado.
- **11 escenarios QEMU: PASS**, según la tabla. Los casos normales de red y HID
  usan dos CPUs; reposo usa 32 MiB. Los discos raíz USB usan 128 MiB, Pentium3,
  TCG y pruebas independientes de checksum, más lanzamiento de File/Processes.
- **2 recompilaciones independientes de fuentes distribuidas: PASS**.
  FAMILYSRC.TGZ produce E100.DVR; PRINTSRC.TGZ produce PRINTSPL.BEX.
- Regenerar el catálogo desde el snapshot produce los mismos hashes y 933 pares
  vendor/device únicos. Compilación C de los cambios sin advertencias. El linker
  mantiene el aviso preexistente de `.note.GNU-stack` en `isr_stubs.o`.

| Escenario | Evidencia dentro de `tests/kernel/evidence/family-expansion-2026-10-13/` | Resultado |
|---|---|---|
| PRO/100 82557B: DHCP/ping | `final/pro100-82557/` | PASS |
| PRO/100 82559C: DHCP/ping | `final/pro100-82559/` | PASS |
| CDC-ECM sobre UHCI: DHCP/ping | `final/usb-ecm-uhci/` | PASS |
| CDC-ECM sobre OHCI: DHCP/ping | `final/usb-ecm-ohci/` | PASS |
| OHCI teclado key-down/key-up y mouse, sin 8042 | `final/usb-hid-ohci/` | PASS |
| Cirrus: nuestro LEGACYFB activo + RGB332 | `final-cirrus-verified/` | PASS |
| Spooler Ring 3: ZJS/JBIG HP1020 | `final/hp1020-render/` | PASS |
| Spooler Ring 3: ID IEEE1284 -> AUTO -> PCL5 | `final/printer-family-auto/`; repetido tras el arreglo de servicio en `final-late-ready-fast/` | PASS |
| Reposo/arranque de imagen con 32 MiB | `final/idle-32m/` | PASS |
| Arranque desde EHCI + 2 lecturas de 1 MiB | `final-ehci-root/` | PASS, checksum `7f65ea70` ambas |
| Arranque desde UHCI + 2 lecturas de 1 MiB | `final-uhci-ready/` | PASS, checksum `7f65ea70` ambas |

## Fallos encontrados y corregidos durante el trabajo

1. PRO/100 recibía pero el filtro exigía flags que QEMU no escribe. Se conserva
   la validación COMPLETE/OK y longitud; el ring usa un marcador de fin móvil
   para no sobrescribir buffers no consumidos. DHCP/ping pasó en dos revisiones.
2. CDC-ECM compartía el lock de RX/TX y podía rechazar DHCP mientras un IN estaba
   esperando. Buffers/locks separados permiten responder desde el callback RX.
3. EHCI observaba un next link terminado antes del writeback del último qTD.
   Esa observación no autoriza completar/reutilizar DMA: ahora espera que tanto
   overlay como qTD final estén inactivos. Arranque y lectura con checksum pasó.
4. Sin i8042, el teclado PS/2 esperaba indefinidamente leyendo `0xff`. Las
   esperas/flush ahora son finitas y el arranque continúa con HID USB. La prueba
   desactiva físicamente ese controlador virtual y verifica USB exclusivamente.
5. El primer smoke Cirrus dibujaba mediante VESA genérico aunque la tabla de
   familias omitía nombres escritos `GD 5446` con espacio. Se corrigió el
   generador y la prueba ahora exige además `[LEGACYFB] Cirrus` en el log.
6. UHCI con lectura fría tardó más de 30 segundos en construir Vision: el
   registro del servicio conservaba SERVICE_START_TIMEOUT y bloqueaba programas
   dependientes, aun después de un escritorio listo. `admin_ready()` acepta
   confirmación tardía solo del mismo PID vivo y solo para ese error. Se conserva
   el evento de timeout, se rechazan otras fallas/PIDs muertos/reemplazados y se
   consulta al scheduler fuera del spinlock. Los programas de prueba arrancaron
   y ambos checksums pasaron. La prueba usa un presupuesto de 600 s por su
   arranque frío USB 1.1 bajo TCG; la primera ejecución de 300 s falló antes de
   este arreglo y no se cuenta como validación aprobada.

El arranque frío medido fue aproximadamente 149 s por UHCI y 24 s por EHCI en
estas fixtures TCG. La lectura de 1 MiB UHCI empleó 137 ticks (100 Hz) en frío
para el archivo y 4 ticks en caché. Estos números no son benchmarks de hardware
físico ni promesa de rendimiento: mejorar el arranque frío USB 1.1 sigue pendiente.

## Reproducir

```sh
make legacy-drivers build/system/drivers/USBCLASS.DVR build/system/services/printspool.o build/programs/texteditor.o build/stage2.bin
python3 tests/drivers/test_family_drivers.py
python3 tests/drivers/test_ohci_transport.py
python3 tests/drivers/test_ehci_completion.py
python3 tests/drivers/test_ps2_optional.py
python3 tests/drivers/test_service_late_ready.py
python3 tests/drivers/test_legacy_fb.py
python3 tests/kernel/run_ram_smoke.py --base build/bleskernos-family-drivers.img --installed --smoke --fixture network --network --network-device i82559c --ram 64 --smp 2 --out /tmp/bk-e100-check
python3 tests/kernel/run_usb_perf.py --base build/bleskernos-family-drivers.img --controller uhci --timeout 600 --out /tmp/bk-uhci-check
```

La imagen se construye con `tools/build_legacy_driver_image.py --output ...`;
el constructor requiere una salida nueva y rechaza sobrescribir la base.
No se ha conectado ninguna impresora física a las pruebas: firmware y papel
HP1020, revisiones PCI/USB reales y los modelos individuales del catálogo
continúan pendientes de validación física. xHCI, hubs/OHCI BOT y periodic/split
EHCI permanecen fuera de esta implementación.
