# Drivers Ethernet históricos

Cada familia tiene un `.c` propio con sus IDs PCI, MAC/EEPROM, registros,
descriptores, recepción/transmisión, portadora y reset:

| Fuente | Módulo |
|---|---|
| `e100.c` | `E100.DVR` |
| `cdc_ecm.c` | `CDC_ECM.DVR` |
| `e1000.c` | `E1000.DVR` |
| `pcnet.c` | `PCNET.DVR` |
| `ne2k_pci.c` | `NE2KPCI.DVR` |
| `tulip.c` | `TULIP.DVR` |
| `rhine.c` | `RHINE.DVR` |
| `sis900.c` | `SIS900.DVR` |
| `natsemi.c` | `NATSEMI.DVR` |
| `rtl8169.c` | `RTL8169.DVR` |

`rtl8139.c` y `3c90x.c` continúan como drivers independientes existentes.

## Qué editar

Para cambiar un chip, modifica su `.c`. Por ejemplo, negociación de enlace de
Tulip en `tulip.c`, o formato de descriptores Intel en `e1000.c`. La función
`supported()` es su tabla de compatibilidad; no agregues IDs sin comprobar los
registros y revisiones de silicio que realmente soporta esa implementación.

Las partes comunes están separadas:

- `legacy_net.h`: contexto privado, tamaños de buffers, acceso a registros,
  esperas acotadas, EEPROM serie y copia/padding de paquetes TX.
- `legacy_net_runtime.inc`: validación y bloqueo de TX, reserva/liberación de
  DMA, registro de la NIC, worker RX y descriptor ABI del módulo.
- `../legacy_io.h`: acceso x86 a puertos, validación de BAR y memoria DMA supervisor.

El runtime se incluye al final de cada `.c` para que contexto, callbacks y
memoria sean privados de su DVR. No selecciona familias mediante `switch` ni
necesita `LEGACY_NET_KIND`; las operaciones específicas están en el archivo
del chip. Un cambio de seguridad común sí alcanza a todos los módulos.

## Compilar y verificar

```
make build/system/drivers/E1000.DVR
make legacy-drivers
python3 tests/drivers/test_legacy_engines.py
```

Las pruebas host ensamblan los mismos archivos del driver y runtime, con I/O
simulado. Cobertura física y limitaciones siguen en
`docs/platform/legacy-drivers.md`; la reorganización no amplía la compatibilidad.

PRO/100 usa RX simplificado, TX síncrono y EEPROM validada por checksum.
Su plantilla Linux está atribuida y hace que E100.DVR sea GPL-2.0-only.
CDC-ECM usa descriptores de clase USB y buffers/locks separados para RX/TX;
no comparte el runtime de NIC PCI. Véase docs/platform/family-drivers.md.
