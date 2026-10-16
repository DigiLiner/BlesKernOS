# Reserva del kernel: 1280 KiB

La reserva anterior era 1024 KiB (2048 sectores). El kernel validado de la fase
anterior ocupaba 1.047.588 bytes: quedaban 988 bytes. La nueva reserva es
1280 KiB (2560 sectores), con 256 KiB adicionales para las primitivas ejercidas
por Wine. No cambia el punto de entrada: Stage 2 carga y salta a 0x00100000.

| Region | Intervalo / restriccion |
|---|---|
| Bounce BIOS de Stage 2 | 0x00010000, transferencias de 16 KiB |
| VGA/ROM BIOS | 0x000A0000..0x00100000, fuera del kernel |
| Archivo kernel | 0x00100000..0x00240000 como maximo |
| BSS | sigue al archivo, alineado a 4096; fin <= 0x003F0000 |
| Pila bootstrap | 0x003F0000..0x00400000 |
| RAM minima | 64 MiB |
| Heap bajo | desde 0x00800000 |
| Vista PE heredada | 0x00400000..0x00800000 |
| Heap alto | desde 0x00800000 |
| VM Linux | desde 0x60000000, con tablas por proceso |

El linker conserva asserts de entrada, imagen plana, BSS y solapamiento.
BSS ya no tiene una base fija de 2 MiB. La tabla de ventanas Win32 (~259 KiB)
se asigna cuando se crea la primera ventana Win32, en vez de ocupar BSS durante
el arranque minimo. Esto preserva margen de heap sin suprimir la ruta heredada.
El limite de archivo no autoriza a invadir BSS/heap: cada incremento futuro
sigue requiriendo linker, comprobacion de layout y boot de 64 MiB.

Stage 2 conserva el buffer bajo y el checksum, y rechaza mas de 2560 sectores.
Makefile, instaladores ATA/USB y floppy, y los harnesses usan la nueva reserva.
En FAT32, los sectores relativos 1..8 contienen Stage 2, 9..2568 el kernel,
2569 FSInfo, 2570 backup BPB y 2571 backup FSInfo; total reservado 2572.
El argumento base del builder es 2569 porque agrega tres sectores de metadata.

`tools/kernel_image.py` migra exclusivamente copias nuevas de las imagenes de
prueba: desplaza FAT y datos, mantiene la numeracion de clusters y actualiza
MBR/BPB/FSInfo. No modifica las imagenes fuente ni discos del usuario.
Los framebuffer MMIO, modulos y frames de VM siguen sus asignadores existentes;
la ampliacion permanece bajo la pila bootstrap y la vista PE historica.

Evidencia historica: `build/layout01-boot4m/result.json` (GUI_RUN con 4 MiB antes del nuevo minimo) y
`build/layout02-check.log` (kernel de 1.051.684 bytes, ya supera el limite viejo).
Las regresiones finales de esta fase se registran en HOST_STATUS.md.
