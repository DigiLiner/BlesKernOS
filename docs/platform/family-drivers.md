# Soporte por familias — 13 de octubre de 2026

Esta ampliación suma motores y rutas de transporte; la meta de 500 dispositivos
no se convierte en 500 drivers. Hay **933 identidades PCI gráficas** en el
catálogo, **23 IDs PCI** en la nueva familia PRO/100 y familias USB/impresión que
se seleccionan por descriptores y capacidades, sin whitelist de fabricante.

**933 IDs no significa 933 placas certificadas.** La ruta gráfica es condicional:
el firmware debe haber establecido un framebuffer VBE válido dentro de un BAR de
memoria de esa placa. El catálogo identifica modelos; el driver comprueba además
la clase PCI, dimensiones, pitch, profundidad y límites de memoria en ejecución.
No hay aceleración, programación de PLLs ni cambio de modo nativo para esas
familias. No se cuenta cada revisión, conector o subsistema OEM como dispositivo
adicional. Las placas físicas distintas probadas en esta ampliación son **cero**;
las pruebas funcionales son en QEMU y las de límites/descriptores usan simulación.

## Código y cobertura

| Área | Fuente | Qué permite |
|---|---|---|
| USB OHCI 1.0 | `kernel/drivers/usb/usb_ohci.c` | Puertos raíz low/full-speed, control, bulk y periodic interrupt: HID, impresoras y CDC-ECM |
| USB EHCI 1.0 | `kernel/drivers/storage/usb_storage.c` + `kernel/drivers/usb/ehci_class.inc` | El motor de almacenamiento ahora publica dispositivos high-speed en la capa común; control y bulk para impresoras/ECM |
| Núcleo USB | `kernel/drivers/usb/usb_uhci.c`, `kernel/include/usb_core.h` | Despacho por HCD, slots reservados atómicamente, locks SMP y ZLP explícito para bulk OUT |
| Ethernet Intel PRO/100 | `kernel/drivers/network/e100.c` / `E100.DVR` | 82557/82558/82559 y determinados 82562 integrados; DMA32, EEPROM con checksum, RX acotado, TX síncrono |
| Ethernet USB estándar | `kernel/drivers/network/cdc_ecm.c` / `CDC_ECM.DVR` | CDC-ECM, unión control/datos, alternate setting, MAC USB, filtro, RX/TX y terminación ZLP |
| Gráficas ATI | `kernel/drivers/graphics/families/ati_vbe_ids.inc` | 407 identidades Rage/Mach64/Rage128/Radeon históricos |
| Gráficas NVIDIA | `kernel/drivers/graphics/families/nvidia_vbe_ids.inc` | 392 identidades RIVA/GeForce/Quadro históricos |
| Gráficas restantes | `kernel/drivers/graphics/families/legacy_vbe_ids.inc` | 134 identidades Cirrus, S3, Matrox, 3dfx, SiS y Trident |
| Familias de impresora | `system/services/print_families.inc`, `USBAUTO.BPD` | Selección por `CMD`/`COMMAND SET` IEEE1284: PostScript, PCL5, ESC/P, ESC/P2; ZJS limitado a HP1020/1018 |

El catálogo completo de modelos y condiciones está en
`docs/platform/framebuffer-device-catalog.json`. Se genera desde el snapshot
licenciado de PCI ID Project con `python3 tools/generate_framebuffer_families.py`.
Principalmente abarca dispositivos de los años 1990/2000 y algunas variantes de
la transición alrededor de 2010. No añade IDs de Ethernet a ciegas ni usa rangos
numéricos de GPU que también aceptarían huecos/desconocidos.

## Impresoras: seleccionar una familia

Para una impresora USB que anuncie un lenguaje compatible, configurar:

```ini
# /SYSTEM/USER/CONFIG/PRINTER.INI
Default=USBAUTO
```

`USBAUTO.BPD` selecciona `USBPRN1`. Se puede copiar el perfil con otro Id/Port
para otra impresora. El SDK y Archivo → Imprimir del Editor usan ese valor.
PostScript se prefiere cuando está anunciado; después PCL5 y ESC/P. PCL6/PCL XL
sin PCL5, GDI/host raster desconocido, PDF y otros formatos se rechazan. No se
supone PCL por ser una HP/Brother y no se envía texto como documento binario.

La HP1020 conserva `Default=HPLJ1020`, el encoder ZjStream/JBIG y el firmware
externo del modelo exacto. No se incluye firmware propietario. No se cambió la
impresora predeterminada del usuario ni se envió nada a una impresora física.
Los perfiles `USBESCP` y `USBESCP2` permiten selección explícita de esas familias.
Un puerto USB solicitado que no existe ya no cae silenciosamente a LPT1.

## Seguridad y límites actuales

- OHCI/EHCI serializan el acceso a sus arenas de DMA y conservan la memoria si
  no pueden confirmar que el hardware dejó de usarla. OHCI espera dos frames
  después de retirar un ED; EHCI sincroniza el unlink y espera el último qTD.
- EHCI acepta lecturas control cortas y dirige su alternate link al STATUS.
  BOT sigue exigiendo exactamente la longitud solicitada.
- UHCI usa CAS entre CPUs. Los slots USB se reservan antes de enumerar, evitando
  que dos HCDs obtengan simultáneamente el mismo objeto libre.
- Un servicio vivo que confirma readiness después de SERVICE_START_TIMEOUT
  puede recuperar su estado y desbloquear dependencias; otros errores no se reviven.
- El arranque no espera indefinidamente un teclado PS/2 ausente; HID USB sigue
  disponible. OHCI respeta intervalos periódicos redondeados a potencias de dos.
- **Hubs downstream: solo UHCI**. OHCI ofrece dispositivos directamente conectados
  a puertos raíz. EHCI high-speed interrupt/isochronous y split transactions no
  están implementados; high-speed HID/hubs no se reclaman como funcionales.
- **OHCI no ofrece almacenamiento BOT ni arranque desde disco USB** todavía.
  El arranque/almacenamiento USB existente continúa con UHCI/EHCI.
- **xHCI/USB 3, RNDIS, CDC-NCM y chips USB Ethernet propietarios: pendientes**.
  Cuando hay ECM en una configuración alternativa, se prefiere esa configuración.
- Una sola NIC activa, sin selección de múltiples interfaces. CDC-ECM usa presencia
  USB como estado de enlace; no interpreta aún NETWORK_CONNECTION del endpoint
  de notificaciones. Jumbo frames/offloads no están habilitados.
- PRO/100 no descarga microcódigo específico ni implementa todas las peculiaridades
  de PHY/WOL/suspend de cada OEM; hay que validar las revisiones físicas.
- Los modelos del catálogo gráfico deben tener VBE/LFB y BAR adecuado. Algunas
  integradas usan RAM robada fuera del BAR y necesitan su backend nativo.

## Pruebas y siguiente paso hacia 500 dispositivos validados

Evidencia en `tests/kernel/evidence/family-expansion-2026-10-13/` y resumen en
`docs/family-expansion-2026-10-13.md`. Se ejercitan DHCP/ping, USB HID real de QEMU,
arranque/lecturas con checksum por EHCI/UHCI, framebuffer Cirrus y renderizado ZJS.
Las pruebas host ejecutan el código de producción con ASan/UBSan o I/O simulado.

La meta de 500 **candidatos de framebuffer** queda cubierta por los 933 IDs.
Para afirmar 500 dispositivos funcionalmente validados, hace falta registrar
modelo, PCI/USB ID, revisión, firmware, controlador host y prueba realizada.
Prioridad siguiente: hubs/OHCI BOT, periodic/split EHCI, familias USB ASIX/RTL8150,
Intel e1000e con PHY/NVM correcto, y pruebas físicas de impresoras por lenguaje.
Mantener separados esos estados evita confundir identificación con funcionamiento.

## Licencias y fuentes

Se estudiaron implementaciones de Linux de OHCI/EHCI, CDC Ethernet, usblp y
vesafb. La lógica nueva usa las ABI de BlesKernOS y no copia esas funciones.
**Sí se copió la plantilla de configuración de 22 bytes de Linux eepro100**:
`e100.c`/`E100.DVR` se distribuyen como GPL-2.0-only, conservando atribuciones a
Donald Becker, Andrey V. Savochkin y los demás desarrolladores del driver.

El snapshot de `pci.ids` y los headers generados usan BSD-3-Clause, con copyright
Martin Mares y Albert Pool. PrintSpool continúa GPL-2.0-or-later por JBIG-KIT.
Referencias/versiones/hashes: `docs/platform/family-driver-sources.json`.

Las imágenes incluyen `FAMILYSRC.TGZ` (fuentes correspondientes y Makefile de
E100.DVR), `NETGPL.TXT`, `PCIIDSLICENSE.TXT`, y `PRINTSRC.TGZ`/`PRINTGPL.TXT`.
Ambos archivos de fuentes se extrajeron y recompilaron independientemente.
