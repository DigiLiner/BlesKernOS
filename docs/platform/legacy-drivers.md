# Drivers históricos: ampliación del 13-10-2026

**Actualización posterior:** [soporte por familias](family-drivers.md) añade
OHCI, clases sobre EHCI, PRO/100, CDC-ECM, selección automática de lenguaje de
impresoras y un catálogo de 933 identidades gráficas condicionales. Las cifras
de diez familias que siguen describen la primera ampliación, antes de esa actualización.

Se incorporan 13 módulos DVR: ocho Ethernet, cuatro PCM y un framebuffer
compartido. Con los motores existentes hay diez familias de audio, diez de
Ethernet y diez familias PCI gráficas cubiertas por el nuevo fallback VBE.
Además se agregan diez perfiles de impresora, incluyendo HP1020 y HP1018.
Un perfil comparte un lenguaje de impresión: no es un controlador USB distinto.
La lista de IDs no equivale a certificación de cada revisión de hardware.

## Ethernet

| Familia | Módulo | PCI representativo | Estado de esta ampliación |
|---|---|---|---|
| Intel 8254x / PRO/1000 | E1000 | 8086:100e; tabla 82542–82546 | DHCP y ping en QEMU |
| AMD PCnet PCI | PCNET | 1022:2000 | DHCP y ping en QEMU |
| NE2000 PCI / RTL8029 | NE2KPCI | 10ec:8029, 1050:0940, 11f6:1401, 8e2e:3000 | DHCP y ping en QEMU RTL8029 |
| DEC 21140/21143 Tulip | TULIP | 1011:0009/0019 | DHCP y ping en QEMU 21143 |
| VIA Rhine I/II/III | RHINE | 1106:3043/3065/3106 | Implementado; hardware pendiente |
| SiS900/7016 | SIS900 | 1039:0900/7016 | Implementado; hardware pendiente |
| National DP83815 | NATSEMI | 100b:0020 | Implementado; hardware pendiente |
| Realtek RTL8169 inicial | RTL8169 | 10ec:8169/8167, 1186:4300 | Implementado; hardware pendiente |
| Realtek RTL8139 | RTL8139 | 10ec:8139 | Motor existente |
| 3Com 3C90x | 3C90X | 10b7:90xx | Motor existente |

RX/TX por polling, buffers DMA de 32 bits supervisor y páginas de guarda.
Los descriptores permanecen en el módulo; TX copia y completa a 60 bytes sin
exponer el buffer del llamador. RX valida longitud, errores y paquete completo;
cada poll procesa como máximo 16 descriptores. Esperas de reset/EEPROM/DMA
acotadas. Una parada que no confirma reset conserva la memoria DMA reservada.
NE2000 serializa el selector de página y DMA remoto entre RX y TX y libera
el bloqueo antes de callbacks que pueden responder con ARP.

La pila del OS admite **una NIC activa**: el primer motor que logra registrarse
la ocupa. No hay selección de varias interfaces, interrupciones, jumbo frames,
offloads ni soporte para todas las revisiones modernas de RTL8168. Tulip arranca
con recepción promiscua y configuración MII/SYM conservadora; no interpreta
todas las tablas de medios de placas antiguas. PCnet, Tulip, NE2000 y SiS900
reportan hardware configurado como enlace disponible; no implementan una
medición completa de portadora por cada variante de PHY. Rhine/SiS/NatSemi/8169
necesitan comprobar PHY, velocidades y revisiones en hardware antes de anunciar
compatibilidad de producción.

## Audio

| Familia | Módulo | PCI / bus | Estado |
|---|---|---|---|
| Ensoniq ES1370 AudioPCI | ES1370 | 1274:5000 | Tono y PCM; WAV capturado en QEMU |
| C-Media CMI8338/8738 | CMIPCI | 13f6:0100/0101/0111/0112 | Implementado; hardware pendiente |
| VIA VT82C686 | VIA686 | 1106:3058 | Implementado; hardware pendiente |
| Cirrus CS4281 | CS4281 | 1013:6005 | Implementado; hardware pendiente |
| Intel ICH / 440MX AC97 | AC97 | tabla Intel existente | Motor existente |
| AMD768 / AMD8111 AC97 | AC97 | 1022:7445/746d | Motor existente |
| NVIDIA nForce AC97 | AC97 | tabla NVIDIA existente | Motor existente |
| SiS7012 AC97 | AC97 | 1039:7012 | Motor existente |
| ESS Maestro3/Allegro | MAESTRO3 | tabla ESS existente | Motor existente |
| Sound Blaster 16 | SB16 | ISA | Motor existente |

Los cuatro motores nuevos ofrecen reproducción PCM u8 mono que se copia y
convierte a estéreo s16, con remuestreo simple a 44100 Hz (ES1370) o 48000 Hz.
Reservan hasta 65536 frames; rechazan clips mayores y frecuencias inválidas.
Los tonos duran hasta 1000 ms. Prioridad 200, por debajo de los motores nativos
preferidos. No ofrecen captura, MIDI, mezcla de voces ni streaming ilimitado;
la finalización se controla por tiempo y polling. VIA686 no se confunde con
VIA8233; ES1370 no se confunde con ES1371 (codec distinto). DMA se libera al
apagar solo si PCI confirma bus master desactivado.

## Framebuffer y pantallas

LEGACYFB reconoce Cirrus GD54xx, S3 Trio64, S3 ViRGE, Matrox Millennium/Mystique/
G200/G400, ATI Rage/Mach64, ATI Rage128, NVIDIA RIVA TNT/TNT2, 3dfx Banshee/
Voodoo3, SiS300/305 y Trident TGUI9440. La tabla exacta está en
`kernel/drivers/graphics/legacy_fb.c`.

**Es un framebuffer de firmware**, no diez motores de cambio de modo nativo.
Requiere que BIOS/VBE haya inicializado un LFB: valida dimensiones 320×200 a
4096×4096, profundidad 8/16/24/32, pitch, overflow y que el área visible quede
completa dentro de un BAR de memoria de la placa reconocida. Conserva únicamente
ese modo. Prioridad 10, sin aceleración; los backends GPU especializados ganan.
VESA.DVR mantiene la conversión de píxeles y la paleta RGB332 para 8 bpp.
QEMU Cirrus GD5446 arrancó Vision en 800×600×8 con este módulo activo.

Esto permite usar el monitor conectado que la BIOS haya seleccionado. No
incluye EDID/DDC, detección individual de modelos de monitor, paneles secundarios,
hotplug, múltiples pantallas ni programación de PLL/LVDS. Las otras nueve
familias necesitan prueba de arranque en hardware con una BIOS VBE funcional.

## Impresoras

Diez perfiles nuevos en `system/printers`:

| Perfil | Dispositivo | Lenguaje | Puerto inicial |
|---|---|---|---|
| HPLJ5 | HP LaserJet 5 | PCL5 | LPT1 |
| HPLJ6L | HP LaserJet 6L | PCL5 | LPT1 |
| HPLJ1100 | HP LaserJet 1100 | PCL5 | LPT1 |
| HPLJ1200 | HP LaserJet 1200 | PCL5 | USBPRN1 |
| HPLJ2100 | HP LaserJet 2100 | PCL5 | LPT1 |
| HPLJ2200 | HP LaserJet 2200 | PCL5 | USBPRN1 |
| HPLJ1020 | HP LaserJet 1020 | ZjStream/JBIG | USBPRN1 |
| HPLJ1018 | HP LaserJet 1018 | ZjStream/JBIG | USBPRN1 |
| EPFX880 | Epson FX-880 | ESC/P, 8 agujas | LPT1 |
| EPLQ570 | Epson LQ-570+ | subconjunto ESC/P compatible | LPT1 |

Se conservan perfiles genéricos y HPLJ4. Selecciona el perfil explícitamente;
el ID IEEE1284 se reconoce y registra, pero no selecciona automáticamente
BPD ni verifica que hayas elegido un lenguaje compatible. Cambia `Port` cuando
corresponda a tu cableado; también pueden dirigirse a archivo para inspección.
Los perfiles PCL/ESC comparten los renderers existentes. Ninguna de estas
impresoras se ha ensayado físicamente en esta sesión.

HP1020/1018: USBCLASS espera firmware del modelo exacto en
`/SYSTEM/FIRMWARE/SIHP1020.DL` o `SIHP1018.DL`, valida su contenedor HP y descarga
en bloques de 4096 bytes, con exclusión mutua y espera de `FWVER:`. Firmware
propietario no incluido; véase `system/firmware/README.md`. ID 03f0:2b17 es 1020,
03f0:4117 es 1018. Los 1000/1005 también tienen carga de firmware identificada,
pero **no** se anuncia soporte de páginas de su variante de protocolo.

El renderer ZJS implementa A4/Letter, simplex, monocromo 600 dpi y páginas
raster de texto ASCII, líneas e imágenes 1 bpp, con filas alineadas a 128 píxeles.
La fuente bitmap es monoespaciada y los estilos son aproximados; no reproduce
las familias tipográficas pedidas. UTF-8 fuera de ASCII y tamaños de papel no
implementados se rechazan. No hay dúplex, selección de bandeja ni administración
de márgenes imprimibles específica de cada impresora; el hardware puede recortar
contenido de los bordes. Las imágenes se remuestrean al DPI del dispositivo.

El spooler corre en Ring 3. Se probaron páginas con el decoder JBIG real y
simulación de firmware ausente, inválido, parcial, timeout y desconexión. Las
pruebas no reemplazan una impresión física ni verifican el transporte de todos
los controladores USB. Tras una transferencia ambigua no se reenvía todo el
trabajo automáticamente; queda como error y requiere reintento manual.

## Referencias y licencias

Implementaciones originales de drivers a partir de registros/descriptores
estudiados en fuentes primarias; no se incorporan archivos de drivers Linux:

- Linux ALSA: [ens1370](https://github.com/torvalds/linux/blob/master/sound/pci/ens1370.c),
  [cmipci](https://github.com/torvalds/linux/blob/master/sound/pci/cmipci.c),
  [via82xx](https://github.com/torvalds/linux/blob/master/sound/pci/via82xx.c),
  [cs4281](https://github.com/torvalds/linux/blob/master/sound/pci/cs4281.c).
- [iPXE pcnet32](https://github.com/ipxe/ipxe/blob/master/src/drivers/net/pcnet32.c),
  [iPXE tulip](https://github.com/ipxe/ipxe/blob/master/src/drivers/net/tulip.c).
- Linux [via-rhine](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/via/via-rhine.c),
  [sis900](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/sis/sis900.c),
  [natsemi](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/natsemi/natsemi.c),
  [r8169](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/realtek/r8169_main.c).
- [QEMU Tulip](https://github.com/qemu/qemu/blob/master/hw/net/tulip.c), usado para contrastar la emulación y el BAR de 128 bytes.
- [OpenPrinting foo2zjs](https://github.com/OpenPrinting/foo2zjs): ZjStream HP1020/1018,
  códigos de items, alineación y empaquetado; USB IDs/FWVER en hplj1000.
- [HP PCL5 Comparison Guide](https://www.hp.com/ctg/Manual/bpl13206.pdf),
  [HP2200 manual](https://h10032.www1.hp.com/ctg/Manual/bpl10361.pdf),
  [Epson FX880](https://files.support.epson.com/pdf/fx880_/fx880_u1.pdf),
  [Epson LQ570+](https://files.support.epson.com/pdf/lq570p/lq570ppg.pdf).

JBIG-KIT sí se incorpora como biblioteca, con sus avisos GPL-2.0-or-later
conservados. PRINTSPL.BEX y su adaptador se distribuyen con esa licencia;
`/DOCS/PRINTGPL.TXT` y `/DOCS/PRINTSRC.TGZ` acompañan la imagen. El kernel no
se enlaza con JBIG-KIT. Procedencia en `third_party/jbigkit/UPSTREAM.json` y
`THIRD_PARTY_LICENSES.md`.

## Reproducir pruebas

```
make legacy-drivers build/system/services/printspool.o build/system/drivers/USBCLASS.DVR build/stage2.bin
make test-legacy-drivers
python3 tests/kernel/run_ram_smoke.py --ram 64 --smoke --fixture network --network --network-device e1000 --drivers-from-build --out tests/kernel/evidence/legacy-drivers-2026-10-13
```

Cambiar e1000 por pcnet, ne2k_pci o tulip prueba cada motor. Audio: fixture
`audio`, `--device ES1370,audiodev=legacy`, `--audiodev wav,id=legacy,path=/tmp/es1370.wav`.
Gráficos: fixture `framebuffer`, `--vga cirrus`. Impresión a archivo con el
spooler real: fixture `printing`. Aislamiento de imports DMA/MMIO: fixture
`driverimports`. Rendimiento de reposo: fixture `idle` con QEMU aislado. El runner siempre usa una copia aislada
de la imagen base y conserva logs/resultados en el directorio de evidencia.
Las pruebas host ejecutan el código de producción con registros simulados y
saneadores; no solamente las tablas PCI.


La imagen de prueba se crea con `python3 tools/build_legacy_driver_image.py`.
Resultados finales y artefactos en `docs/driver-expansion-2026-10-13.md` y
`tests/kernel/evidence/legacy-drivers-2026-10-13/summary.json`. Se conservan
los hashes de las referencias consultadas en `legacy-driver-sources.json`.


## Organización del código

Los ocho motores Ethernet se mantienen en archivos independientes de
`kernel/drivers/network/`: `e1000.c`, `pcnet.c`, `ne2k_pci.c`, `tulip.c`,
`rhine.c`, `sis900.c`, `natsemi.c` y `rtl8169.c`. Los cuatro de sonido están
en `kernel/drivers/audio/`: `es1370.c`, `cmipci.c`, `via686.c` y `cs4281.c`.
Los antiguos archivos `legacy_pci_net.c` y `legacy_pci_audio.c` fueron
reemplazados. Contextos, acceso común a registros, comprobaciones y gestión
DMA/PCM se comparten sin mezclar la lógica de distintos chips. Véanse los
README de cada directorio para elegir el archivo que corresponde mejorar.
