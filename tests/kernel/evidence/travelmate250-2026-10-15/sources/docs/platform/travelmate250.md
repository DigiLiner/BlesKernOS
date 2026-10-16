# Acer TravelMate 250: soporte de hardware

Implementación del 15 de octubre de 2026. Se trata del TravelMate **250 / serie
240–250**, no del 2500, que usa otro chipset gráfico. La identificación siguiente
procede de la guía de servicio de Acer; todavía debe confirmarse con el PCI del
portátil del usuario, especialmente si tiene opciones regionales o piezas cambiadas.

| Función | Hardware de la configuración de referencia | Soporte |
|---|---|---|
| Vídeo | Intel 852GME, familia 852/855, PCI 8086:3582 | INTELGMA.DVR |
| Audio | Intel ICH4-M AC'97, PCI 8086:24c5, codec Cirrus CS4299-XQ | AC97.DVR |
| Ethernet | Realtek RTL8101L 10/100, núcleo RTL8139 | RTL8139.DVR + NETSTACK.DVR |
| Disco | IDE del ICH4 | Driver ATA existente |
| Entrada | Teclado y touchpad PS/2 | Soporte PS/2 existente |

El Wi-Fi mini-PCI es opcional; no se presume un modelo ni se anuncia soporte nuevo
para él. CardBus, módem, suspensión y control térmico no se certifican en esta ronda.

## Vídeo

Se agregó 8086:3582 a la tabla real. Gen2 usa apertura en BAR0 y MMIO en BAR1,
a diferencia de los GMA posteriores. Se conserva el modo programado por el BIOS,
la selección del plano LCD, su stride y su offset. No se reemplazan PLL/LVDS ni
se anuncian cambios arbitrarios de resolución: sólo el modo activo del firmware.

Aceleración 2D de relleno y copia, incluidos solapamientos mediante una fila
intermedia, a 16/32 bpp. Se exige una prueba real fuera de pantalla de COLOR_BLT y
SRC_COPY_BLT antes de publicar capacidades. El CPU espera a las operaciones GPU
pendientes antes de escribir el framebuffer.

En Gen2 se reserva una arena propia de 8 MiB y se mapea en la cola de GGTT. No se
usa memoria robada del BIOS como si fuera libre. Se verifican los planos activos,
se conservan las entradas GTT originales y se usan accesos CPU por la apertura
sin caché. Un fallo del motor desactiva aceleración; se conserva la memoria que
pueda seguir referenciada por DMA hasta reiniciar.

El backend 3D Gen2 implementa superficies ARGB/XRGB, triángulos ya transformados
con color Gouraud, Z16 con funciones de comparación, mezcla alfa, transferencias
de color/profundidad y presentación. Se integra mediante la ABI GFX3D con el
backend existente de Mesa 3.5 que usan los protectores. Se habilita sólo en modo
32 bpp y después de una autoprueba GPU de color, profundidad y mezcla. Las
texturas, shaders, operaciones GL no soportadas y la falta de memoria permanecen
en software. Esto **no es un driver OpenGL completo ni una certificación de
aceleración en el equipo físico**. QEMU no emula esta GPU; la autoprueba en el
TravelMate y una sesión real de los protectores siguen pendientes.

## Audio

El driver reconoce el ICH4 y ahora puede usar sus BAR2/BAR3 MMIO nativos, además
de los BAR0/BAR1 de E/S. Los registros MMIO se mapean sin caché y conservan su
dirección completa. Se contempla retorno de PCI D3 a D0, reset frío/caliente del
AC-link, encendido de DAC/mixer/amplificador, desmute de master/headphone/mono/PCM,
salida fija estéreo de 16 bits a 48 kHz y el valor AC mode del CS4299. La etapa
PCM y sus mezclas existentes se conservan. Captura y ajustes OEM particulares
no se consideran certificados por estas pruebas.

## Ethernet, DHCP y DNS

- Se corrige la copia de paquetes que cruzan el límite: RxNoWrap escribe el
  resto del paquete en la extensión del buffer, no al inicio del anillo.
- RX de 32 KiB con extensión, base alineada y sondeo cada tick de hasta 1 ms,
  recuperación de overflow y de un DMA que no termina.
- Se programa RCR después de habilitar RX/TX. Se configura TCR y se corrige
  el desplazamiento del umbral FIFO TX (bits 16+, no bits 11+). El umbral se
  adapta ante underrun. El estado de enlace se lee dos veces por su latch-low.
- DHCP retransmite DISCOVER y REQUEST, preserva gateway/máscara/DNS de la oferta
  cuando ACK no los repite, rechaza respuestas truncadas/ajenas/tardías y evita
  que dos solicitantes mezclen transacciones. Añade client-id y tamaño máximo.
- Resolver una IPv4 literal ya no requiere que exista un servidor DNS configurado.
  Si falta DNS se permiten las alternativas ya existentes del resolver.

La renovación automática de leases y la conformidad completa DHCP/DNS no se
implementaron en esta ronda. Las pruebas no garantizan conexión con todos los
routers; los resultados del portátil decidirán los siguientes ajustes.

## Escritorio

Classic se decodificaba pero se descartaba al intentar cambiar su propietario
con un PID desde Vision: la API de metadatos de usuario rechaza esa operación.
El cache permanece ahora en el heap del servicio que lo creó, con su vida útil
correcta y sin ampliar permisos. Se conserva la opción de cambiar de fondo.
Classic también se incluye en el bootstrap del CD, antes de desempaquetar ASSETS.
El deskbar queda fijo en su ancho mínimo de 620 píxeles, o el ancho disponible
en pantallas menores; sus asas decorativas ya no permiten agrandarlo.

## Pruebas

Los resultados y fuentes exactos se archivan en
`tests/kernel/evidence/travelmate250-2026-10-15/`.

- ASan/UBSan: DHCP con pérdida del primer paquete de ambas fases, ACK sin
  opciones, paquetes malformados, servidor ajeno y exclusión de solicitantes.
- ASan/UBSan: RX cruzando el anillo, DMA incompleto y estancado, padding TX y
  bits correctos/adaptación del umbral FIFO.
- ASan/UBSan, nativo e i386: arena 3D, agotamiento/reuso de superficies, pitch
  de color/Z16, comandos 2D de 16/32 bits y 3D, rechazo de NaN/Inf y batching.
  Esto valida software y paquetes, **no rasterización física**.
- ASan/UBSan i386: accesos MMIO AC'97 de 8/16/32 bits, PMCSR D3→D0 sin borrar
  PME status y recorrido acotado de capacidades PCI. Fragmentos PCM: PASS.
- QEMU KVM, 256 MiB, una CPU: RTL8139 obtiene IP/gateway/DNS, ping y resolución
  de example.com: PASS. Esa prueba DNS requiere un DNS upstream operativo.
- QEMU AC'97: tono y PCM, captura WAV con muestras no nulas: PASS. QEMU usa
  ICH con registros de E/S; el ICH4 MMIO y CS4299 físicos siguen pendientes.
- Captura de escritorio: mosaico Classic comparado con sus píxeles originales;
  deskbar compacto. Protectores SSPIPES y SSBALLS animan y salen con Escape.
  Estas pruebas QEMU no certifican velocidad ni aceleración de la GPU Intel.

Para repetir las pruebas del software:

```sh
python3 tests/kernel/test_travelmate_network.py
python3 tests/kernel/test_ac97_hardware_access.py
python3 tests/kernel/test_intel_gen2.py
python3 tests/desktop/test_ac97_fragments.py
python3 tests/kernel/run_ram_smoke.py --base build/bleskernos-travelmate250.img \
  --fixture network --smoke --ram 256 --accel kvm --cpu host --smp 1 \
  --network --network-dns-test --network-device rtl8139 --drivers-from-build \
  --out build/travelmate250/mi-prueba
```

## Probar en el portátil

Se entregan una imagen normal `build/bleskernos-travelmate250.img` y un CD de
instalación `build/bleskernos-travelmate250.iso`. La imagen Wine anterior se conserva.
Estas son imágenes completas, no actualizadores que deban escribirse sobre una
instalación con datos. El CD permite probar el procedimiento de instalación.

Desde la consola del sistema: `pci`, `bootlog 200`, `net info`, `net dhcp`,
`dns example.com`, `nettest example.com` y `soundtest`. El informe del arranque
permite ver INTELGMA, plano/stride, la autoprueba 2D/3D y el codec de audio.
Comprobar sonido por altavoces y auriculares, mover ventanas, abrir/cerrar los
protectores y sostener una sesión de red. Un escritorio visible mediante el
fallback CPU no demuestra que haya pasado la autoprueba 3D.

## Fuentes primarias

- [Guía de servicio Acer 240/250](https://www.opweb.de/pdf/acer--travelmate-250-series--service--ID11004.pdf).
- [Datasheet Intel 855GM/GME](https://www.intel.com/content/dam/doc/datasheet/855gm-gme-chipset-graphics-memory-controller-hub-datasheet.pdf).
- [Mesa 7.11 i830_reg.h](https://github.com/notaz/mesa/blob/mesa-7.11/src/mesa/drivers/dri/i915/i830_reg.h)
  y [i830_state.c](https://github.com/notaz/mesa/blob/mesa-7.11/src/mesa/drivers/dri/i915/i830_state.c).
  Las definiciones de paquetes conservan su aviso MIT/Tungsten en los headers.
- [Linux intel-gtt](https://github.com/torvalds/linux/blob/master/drivers/char/agp/intel-gtt.c),
  [intel8x0](https://github.com/torvalds/linux/blob/master/sound/pci/intel8x0.c),
  [codec Cirrus](https://github.com/torvalds/linux/blob/master/sound/pci/ac97/ac97_patch.c)
  y [8139too](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/realtek/8139too.c).
