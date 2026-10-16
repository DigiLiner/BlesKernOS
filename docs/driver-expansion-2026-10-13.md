# Ampliación de drivers históricos — 13-10-2026

Implementación terminada y compilada: 13 módulos nuevos (8 Ethernet, 4 audio,
1 framebuffer compartido), USBCLASS ampliado y 10 perfiles nuevos de impresora.
Con los motores existentes: 10 familias Ethernet y 10 de audio. LEGACYFB cubre
10 familias PCI mediante el framebuffer que inicializó BIOS/VBE.

La [matriz de compatibilidad](platform/legacy-drivers.md) enumera IDs, puertos,
funciones y límites. El catálogo es de implementaciones, no de hardware
certificado: PHY y variantes de placas reales todavía requieren ensayos.

## Resultado verificable

- 16 ejecutables host de código de producción: 8 NIC, 4 audio, framebuffer,
  firmware HP, ID IEEE1284 y renderer ZJS. Pasaron validaciones de límites,
  copia de buffers, errores de RX, ring lleno, conversión PCM, cancelación,
  firmware inválido/parcial/desconectado y documentos malformados.
- Regresiones del cargador ELF y frontera de memoria de la API nativa: PASS.
- Intel8254x, PCnet, NE2000PCI y Tulip: DHCP y ping con 64 MiB y cuatro CPU
  en QEMU. Cada ejecución recibió y transmitió seis paquetes.
- ES1370: tono y PCM capturados a WAV, estéreo s16 a 44100 Hz. La captura
  contiene tanto el tono ±4096 como el PCM con ganancia ±2891.
- Cirrus GD5446: Vision arrancó en 800×600×8 con LEGACYFB activo. Escritura y
  lectura de rojo, verde y azul en RGB332: PASS. Captura del escritorio guardada.
- PrintSpool en Ring 3 generó una página ZjStream/JBIG A4. El decodificador
  upstream `zjsdecode` de OpenPrinting reconoció sus records y produjo una
  imagen PBM de 4992×7016: píxeles negros en (600,600)/(601,601) y blanco
  en (602,602), según el bitmap de entrada.
- Un programa Ring 3 intentó importar débilmente las tres nuevas funciones
  DMA/MMIO de supervisor: los tres símbolos quedaron sin resolver, como debe
  ocurrir. El permiso de imports depende de la imagen que carga el ELF y no
  de una bandera global compartida entre CPU.
- El test de reposo de Cirrus pasó en ejecución aislada (Vision 20%). Un
  ensayo concurrente con otros QEMU llegó a 36% y falló su presupuesto de 20%;
  ambos resultados se conservan. No es una medición de rendimiento físico.

Son nueve pruebas funcionales QEMU finales aprobadas, además del ensayo
concurrente de reposo fallido que se indica arriba. Evidencia y hashes en
`tests/kernel/evidence/legacy-drivers-2026-10-13/summary.json`, con logs, PNG,
WAV, ZJS y PBM. No se conectó una impresora ni otra placa física.

## Usar y reconstruir

Imagen arrancable separada: `build/bleskernos-legacy-drivers.img` (BFS4, 768 MiB
lógicos). Incluye kernel, módulos, perfiles, spooler, documentación, licencia
y fuentes de PrintSpool. La imagen base conserva su contenido original.

```
make legacy-drivers build/system/drivers/USBCLASS.DVR build/system/services/printspool.o build/stage2.bin
make test-legacy-drivers
python3 tools/build_legacy_driver_image.py --output build/mi-imagen-drivers.img
```

El constructor parte de una imagen BFS4 existente y crea otra, conservando
programas y preferencias. Rechaza sobrescribir la imagen base o una salida
existente. El empaquetador normal también instala los DVR y BPD nuevos.

HP1020/1018: colocar el archivo del modelo exacto en
`/SYSTEM/FIRMWARE/SIHP1020.DL` o `SIHP1018.DL`. No se incluye firmware
propietario. [Instrucciones](../system/firmware/README.md). El renderer actual
es monocromo, simplex A4/Letter, 600 dpi, con texto ASCII y fuente bitmap
aproximada; falta comprobar impresión física y márgenes de hardware.

PrintSpool enlaza JBIG-KIT GPL-2.0-or-later; su licencia y sus fuentes
recompilables están en `/DOCS/PRINTGPL.TXT` y `/DOCS/PRINTSRC.TGZ`. El Makefile
autónomo del archivo de fuentes también fue compilado con éxito. El kernel y
los módulos DVR no enlazan JBIG-KIT. Referencias consultadas y sus hashes en
[legacy-driver-sources.json](platform/legacy-driver-sources.json).
