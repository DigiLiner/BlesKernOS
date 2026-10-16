# Self-hosting en BlesKernOS 0.9

> Tras retirar la capa Win32 propia, las herramientas `.EXE` se ejecutan con
> Wine mediante `wine-tool.ELF`. La reconstrucción completa descrita más abajo
> está pendiente de adaptación y validación: el makefile nativo dependía de
> un intérprete de recetas que lanzaba comandos BEX desde procesos Windows.
> Eliminar esa dependencia no garantiza todavía `make selfhost` dentro del SO.


Todas las ediciones completas User y Developer contienen el SDK y el toolchain
en `/SYSTEM/SDK` y `/SYSTEM/DEVEL`: ATA, USB, VMware y las imagenes de
instalacion derivadas de ATA. El disquete de rescate de 1.44 MiB queda excluido
porque el toolchain por si solo supera su capacidad. Se incluyen dos variantes
de TinyCC 0.9.28rc:

- `I386-TCC.EXE` se ejecuta mediante Wine en BlesKernOS y genera ELF32
  i386 relocatable, el formato de las aplicaciones nativas `.BEX`.
- `TCC.EXE` genera aplicaciones PE32 para la capa Win32.

Los comandos `gcc` y `cc` ofrecen el frontend compatible habitual. Para una
aplicación nativa de un solo archivo, el comando recomendado es:

```text
build HELLO.C HELLO.BEX
start HELLO.BEX
```

La forma explícita equivalente es:

```text
compile -I/SYSTEM/SDK/INCLUDE HELLO.C -o HELLO.O
link HELLO.O /SYSTEM/SDK/LIB/LIBBLESK.A -o HELLO.BEX
```

Un programa nativo usa `#include <bleskernos_api.h>` y define
`void bleskernos_program_main(void *unused)` como punto de entrada.

GNU Binutils 2.47 aporta el ensamblador, enlazador y utilidades ELF completas:
`as`, `ld`, `ar`, `ranlib`, `objcopy`, `objdump`, `readelf`, `nm`, `size`,
`strip`, `strings`, `addr2line`, `cxxfilt` y `elfedit`. `gcc -c` compila; sin
`-c`, el wrapper genera un único ELF relocatable mediante `ld -r`.

## NASM, Makefiles e imagenes FAT32

`nasm` ejecuta NASM 3.02 completo y `ndisasm` su desensamblador. Por ejemplo:

```text
nasm -f bin /SYSTEM/SOURCE/BOOT/BOOT_FAT32.ASM -o BOOT.BIN
nasm -f elf32 /SYSTEM/SOURCE/KERNEL/ENTRY.ASM -o ENTRY.O
```

GNU Make 4.4.1 esta disponible con el comando `make`. El ejemplo incluido se
construye asi:

```text
cd /SYSTEM/SDK/EXAMPLES/HELLO
make
start HELLO.BEX
```

El port conserva variables, reglas explicitas e implicitas, dependencias,
targets `.PHONY`, funciones GNU, includes y comprobacion de fechas. Las recetas
se ejecutan en serie y heredan la consola de BlesKernOS; todavía no hay pipes
anonimos ni un shell POSIX, por lo que recetas con `|`, redirecciones complejas,
`for`, `if` o sustituciones `$(shell ...)` deben expresarse como comandos BEX.

`fatpack` reemplaza las operaciones usadas de `mkfs.fat` y `mtools`:

```text
fatpack create BLES.IMG 65536 2057
fatpack mkdir BLES.IMG /SYSTEM
fatpack add BLES.IMG HELLO.BEX /SYSTEM/PROGRAMS/HELLO.BEX
fatpack raw BLES.IMG 1 STAGE2.BIN 8
fatpack raw BLES.IMG 9 KERNEL.BIN 2048
fatpack boot BLES.IMG BOOT_FAT32.BIN
```

El empaquetador escribe FAT32, dos FAT, FSInfo, backup del boot sector,
directorios expandibles, nombres 8.3 y nombres largos VFAT.

## Estado del bootstrap

La edición Developer contiene el árbol completo en `/SYSTEM/SOURCE`, incluido
`mk/selfhost.mk`. Desde ese directorio se puede reconstruir cada parte o producir
una imagen FAT32 ATA arrancable:

```text
cd /SYSTEM/SOURCE
make -f mk/selfhost.mk selfhost-kernel
make -f mk/selfhost.mk selfhost-sdk
make -f mk/selfhost.mk selfhost-programs
make -f mk/selfhost.mk selfhost-image
```

El último target genera `build/BLES-SELFHOST.IMG`: crea FAT32, copia el sistema
instalado y reemplaza boot, kernel, programas, comandos, drivers, paneles,
servicios y bibliotecas SDK por los resultados recién compilados. `make -f
mk/selfhost.mk selfhost` es el atajo para el proceso completo.

Para preparar un disco separado con el árbol fuente del OS se puede ejecutar
desde el repositorio:

```text
make source
```

Esto genera `build/source/bleskernos-source.img`, una imagen FAT32 de 96 MiB con
estructura de arranque y `/SYSTEM/SOURCE` (incluye `boot`, `kernel`, `gui`,
`programs`, `system`, `sdk`, `libs`, `assets` y `tools`). La carpeta `programas
extras` no se copia; el disco está pensado para montarse desde una edición
completa y copiar/reconstruir el sistema.

User conserva el mismo compilador, Binutils, NASM, Make, FATPACK y SDK, pero no
duplica el árbol fuente del sistema para ahorrar espacio. Por eso puede compilar
programas y SDK propios; para recompilar el OS completo debe copiarse el árbol
fuente o partir de Developer.

`mk/selfhost.mk` sustituye las recetas del Makefile de desarrollo que dependen de
un shell Unix, Python, mtools, `dd` o `wc`. Las conversiones opcionales a ISO y
VMDK continúan siendo tareas del host; la salida nativa es la imagen FAT32 ATA.

TinyCC se obtiene desde un commit fijado y se distribuye bajo LGPL-2.1 o
posterior. Su licencia se instala como `/SYSTEM/DEVEL/TCC/COPYING.TXT`.
NASM se distribuye bajo BSD de dos clausulas y GNU Make y GNU Binutils bajo
GPLv3 o posterior. Las licencias se instalan junto a cada herramienta.
