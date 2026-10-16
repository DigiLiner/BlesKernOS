# Drivers separados por familia

Los ocho motores Ethernet y los cuatro motores PCM añadidos el 13-10-2026
se reorganizaron en doce archivos fuente independientes. No se cambiaron
los dispositivos admitidos ni los nombres de los módulos DVR.

```
kernel/drivers/
  network/
    e1000.c       pcnet.c       ne2k_pci.c    tulip.c
    rhine.c       sis900.c      natsemi.c     rtl8169.c
    legacy_net.h
    legacy_net_runtime.inc
    README.md
  audio/
    es1370.c      cmipci.c      via686.c      cs4281.c
    legacy_pcm.h
    legacy_pcm_runtime.inc
    README.md
```

Para mejorar un chip, edita su `.c`: contiene IDs PCI, selección de BAR,
registros y operaciones de hardware. Los headers comparten acceso a registros
y contexto; los runtime comparten validaciones, memoria DMA, bloqueos,
conversión PCM y ciclo de vida. Cada módulo compila su propia instancia privada
de esos helpers; no hay una memoria global compartida entre distintos DVR.

No quedan seleccionadores `LEGACY_NET_KIND`/`LEGACY_AUDIO_KIND` en los drivers.
El Makefile compila cada fuente directamente. Por ejemplo:

```
make build/system/drivers/PCNET.DVR
make build/system/drivers/ES1370.DVR
make legacy-drivers
python3 tests/drivers/test_legacy_engines.py
```

La lógica VBE de `graphics/legacy_fb.c` permanece compartida porque es un
backend de framebuffer de firmware. Los drivers gráficos nativos ya tienen
sus fuentes propias. Los perfiles BPD de impresora están separados por modelo;
HP1020/1018 comparten el renderer ZjStream porque usan la misma variante.

## Validación de la reorganización

- Doce módulos compilados sin advertencias nuevas.
- Doce ejecutables host: descriptores RX/TX, límites, ring lleno, copia/padding,
  errores y callbacks reentrantes; conversión PCM y parada de tonos: PASS.
- Intel8254x, PCnet, NE2000PCI y Tulip: DHCP y ping en QEMU con cuatro CPU: PASS.
- ES1370: tono y PCM en QEMU, con WAV de 44100 Hz estéreo y niveles esperados: PASS.

Evidencia en `tests/kernel/evidence/driver-source-split-2026-10-13/summary.json`.
Estos ensayos comprueban que la separación conserva las funciones probadas;
no añaden certificación de hardware físico. La imagen
`build/bleskernos-legacy-drivers.img` incluye los módulos reorganizados.

## Ampliación posterior por familias

PRO/100 está en `network/e100.c` (GPL-2.0-only), CDC-ECM en `network/cdc_ecm.c`
y OHCI en `usb/usb_ohci.c`. El transporte de clases EHCI se comparte con el
motor BOT mediante `usb/ehci_class.inc`. Las identidades gráficas se separan en
`graphics/families/*_vbe_ids.inc` y se regeneran desde un snapshot licenciado.
La selección de lenguaje de impresoras está en `system/services/print_families.inc`.
Detalles y licencias: `docs/platform/family-drivers.md`.
