# Organización de controladores

Los controladores se agrupan por el subsistema que implementan:

- `core/`: ABI y buses internos (`gfx`, `gfx3d`, PCI, block, VFS y PIT).
- `graphics/`: VGA/VESA, VMware SVGA, VirtIO GPU, ATI Rage 128 e Intel GMA.
- `audio/`: núcleo de sonido y controladores SB16, AC97 y Maestro3.
- `network/`: NIC, pila IPv4 y proveedor TLS.
- `storage/`: ATA, floppy, sistemas de archivos y almacenamiento USB.
- `input/`: teclado y mouse.
- `usb/`: host UHCI y clases USB.
- `platform/`: RTC y puerto paralelo.
- `archive/`: copias históricas que no participan del build.

Los módulos externos se compilan como `build/system/drivers/*.DVR`. Los
componentes residentes se enlazan dentro del kernel. Al mover o agregar un
archivo hay que actualizar `KERNEL_SOURCES` o el target `.DVR` correspondiente
en el `Makefile`.


La ampliación de dispositivos de 1990/2000 incorpora E1000, PCNET, NE2KPCI,
TULIP, RHINE, SIS900, NATSEMI, RTL8169, ES1370, CMIPCI, VIA686, CS4281 y
LEGACYFB. Compilar con `make legacy-drivers`; pruebas host con
`make test-legacy-drivers`. Cobertura, IDs, referencias, pruebas QEMU y límites
en `docs/platform/legacy-drivers.md`. LEGACYFB depende de un modo BIOS/VBE
válido; no programa modos nuevos ni ofrece aceleración.


## Fuentes separadas por familia

Los drivers Ethernet nuevos están en `network/e1000.c`, `pcnet.c`, `ne2k_pci.c`,
`tulip.c`, `rhine.c`, `sis900.c`, `natsemi.c` y `rtl8169.c`.
Los de sonido están en `audio/es1370.c`, `cmipci.c`, `via686.c` y `cs4281.c`.
Cada archivo contiene la implementación del hardware; acceso a memoria,
validaciones y ciclo de vida se comparten en los headers/runtime del subsistema.
Mapas de fuentes y explicación para mantenerlos en `network/README.md` y
`audio/README.md`. Los nombres de los módulos `.DVR` se mantienen.

Los perfiles de impresora ya están separados por modelo en `system/printers`.
LEGACYFB sigue siendo un único backend de firmware VBE: las diez familias usan
el mismo framebuffer, sin diez motores distintos de programación del hardware.
Los backends gráficos nativos ATI/Intel/VMware/VirtIO están separados.
