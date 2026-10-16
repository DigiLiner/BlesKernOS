# Wine upstream para BlesKernOS i386

Juegos de 32 bits (2026-10-15): [adaptación de superficies, runtime VirGL y pruebas Direct3D/OpenGL](evidence/games32-2026-10-15/README.md).

Actualización de compatibilidad (2026-10-15): [notificaciones de archivos y
pruebas Windows/Linux](evidence/compat-notify-2026-10-15/README.md). El texto de
W3 que sigue conserva el registro histórico de esa etapa.

**W3 aprobada para la inicialización del prefijo de Wine 11.0 y su
persistencia.** El primer proceso PE deja que ntdll ejecute automáticamente
`wineboot.exe --init`; el gate comprueba claves del registro, escribe un
marcador, espera que wineserver guarde los tres hives y reinicia QEMU con una
copia de la imagen para leer el marcador sin recrearlo. Con KVM, AHCI, una CPU
y 1536 MiB de RAM, el primer gate tardó 277,52 s y el reinicio 482,46 s.
Los logs y resultados están en `build/w3-auto-init-kvm-1536-01/` y
`build/w3-auto-init-kvm-1536-reboot-01/`. X11 y aplicaciones Windows con
interfaz gráfica siguen fuera de esta validación; la ruta Win32 heredada
permanece disponible.

La bateria glibc tiene 30 casos: la base anterior de 27, mas MAP_SHARED,
locks POSIX y realpath/readlink. Los resultados y limites exactos estan en
[W3_WINEBOOT_WORKLOG.md](docs/W3_WINEBOOT_WORKLOG.md). Los documentos
[HOST_STATUS.md](docs/HOST_STATUS.md) y [KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md)
conservan el estado de etapas anteriores.

En WSL/Linux, desde la raiz del repositorio:

```sh
python3 ports/wine/scripts/build-runtime.py
python3 ports/wine/scripts/build-wine.py --work /home/lucas/blesk-wine-nuevo --jobs 3
python3 ports/wine/scripts/package-bootstrap.py --work /home/lucas/blesk-wine-nuevo
python3 ports/wine/scripts/run-runtime.py --cases 30 --program /WINEGATE/bin/wine-launch --expected-text wine-11.0 --case-seconds 60 --output ports/wine/build/mi-w1
```

Estado de integración actualizado: consultar [W3 worklog](docs/W3_WINEBOOT_WORKLOG.md).
La inicialización sólo se aprueba cuando el launcher termina con código 0,
consulta el registro y guarda los tres hives. Para validar su persistencia
con un arranque nuevo de QEMU, después de un gate W3 aprobado:

```sh
python3 ports/wine/scripts/run-runtime.py \
  --image ports/wine/build/full03/test.img \
  --rootfs ports/wine/build/current-rootfs \
  --cases 23,25,28,30 --wineboot --wineboot-full-runtime \
  --wineboot-image-mib 768 --boot-ahci --accel kvm \
  --ram-mib 1536 --case-seconds 1200 --seconds 1200 \
  --output ports/wine/build/mi-w3
```

```sh
python3 ports/wine/scripts/verify-prefix.py --run ports/wine/build/mi-w3 --output ports/wine/build/mi-w3-reboot --normal-start
```

Para medir la segunda vuelta sobre el mismo archivo, agregar `--in-place` a
ese comando. La prueba directa registrada en la bitácora tardó 240,35 s en
el primer gate y 372,12 s en el segundo.

El segundo runner copia el disco del primer gate por defecto; `--in-place`
arranca exactamente el mismo `test.img` y reemplaza dentro de él sólo el
launcher de prueba. Conserva el kernel y exige un marcador de registro
escrito en el primer arranque. No lo vuelve a crear.
Wine vuelve a ejecutar su arranque de servicios al iniciar el primer proceso
de cada arranque de QEMU; esto no borra ni recrea el prefijo.
Un primer gate fallido no habilita esta comprobación.

Usar una ruta persistente de build sin espacios y un directorio nuevo por prueba.
Wine está fijado por tag/SHA256 en `versions/wine-11.0.json`. El build aplica
el [parche de espera de wineboot](patches/0001-wineboot-allow-30-minutes.patch),
que amplía de 5 a 30 minutos la espera inicial solicitada para este port.
El manifiesto registra el hash del parche; ampliar el plazo no aprueba el gate.
También aplica el [reconocimiento de FAT](patches/0002-ntdll-recognize-fat-case-insensitivity.patch)
para evitar búsquedas redundantes de nombres. El kernel admite 512 descriptores
por proceso, incluidos sockets y tuberías; la tabla se asigna por proceso para
conservar memoria para los procesos de Wine; el minimo de arranque es 64 MiB.
Para reconstruir sólo ese módulo sin las dependencias de desarrollo gráficas,
usar `build-wine.py --ntdll-only --work /ruta/nueva/sin/espacios`. El resultado
`obj/dlls/ntdll/ntdll.so` puede probarse con `run-runtime.py --wine-ntdll` junto
con las opciones de W3. Ese build parcial no sustituye un build completo de Wine.
Los wrappers locales de assembler/linker fuerzan ELF i386 tambien para
las import libraries que genera winebuild. `resume-wine.py` conserva objetos,
invalida import libraries ELF64 identificadas y reanuda build/install en DESTDIR.
`package-wine.py` empaqueta la instalacion completa, dependencias, symlinks,
licencias y hashes. El bootstrap W1 tiene un manifiesto separado.

Si se conserva el paquete completo anterior y se reconstruye sólo `ntdll.so`,
`package-validated-runtime.py` permite generar otro paquete con el runtime
exactamente probado. Exige los resultados aprobados del prefijo nuevo y del
reinicio, verifica hashes y conserva el paquete base:

```sh
python3 ports/wine/scripts/package-validated-runtime.py \
  --base ports/wine/package/wine-11.0-i386 \
  --runtime ports/wine/build/current-rootfs \
  --module ports/wine/build/fatcase-ntdll \
  --run ports/wine/build/mi-w3 --reboot ports/wine/build/mi-w3-reboot \
  --output ports/wine/package/wine-11.0-i386-w3
```

El empaquetado cambia PT_INTERP/RUNPATH de Wine para usar glibc en `/WINEGATE/lib`;
registra hashes originales y finales. No modifica libc ni ld.so. Esto permite
conservar el interprete didactico usado por los ELF heredados en `/lib`.

Dependencias de build: GCC/G++ multilib, flex, bison, make, NASM, patchelf, mtools,
QEMU y paquetes de desarrollo i386 X11, Xext, Xrender, Xrandr, Xi, Xcursor,
Xfixes, FreeType y Fontconfig. El manifiesto registra versiones instaladas.
No es un build hermetico ni una promesa de reproducibilidad bit a bit.

Regresiones:

```sh
python3 ports/wine/scripts/run-runtime.py --legacy --cpus 2 --case-seconds 60 --output ports/wine/build/mi-smp
python3 tools/run_office95_validation.py --output ports/wine/build/mi-ole
python3 ports/wine/tests/boot.py --output ports/wine/build/mi-boot64m
python3 tools/check_high_kernel_layout.py
python3 tests/kernel/test_kernel_reserve.py
```

Cada runner utiliza una copia nueva del disco. Se conserva la imagen fuente.
`--trace 1` registra errores y `--trace 2` todas las llamadas; usar la segunda
opcion solo para diagnosticos breves. Evitar varios guests de validacion a la
vez: hay evidencia de timeouts ATA bajo carga concurrente, conservada en los logs.

Ver tambien [layout del kernel](docs/KERNEL_LAYOUT.md),
[cobertura de syscalls](docs/SYSCALL_STATUS.md), [arquitectura](docs/ARCHITECTURE.md)
y [Office 95](docs/OFFICE95_STATUS.md).

El parche `0006-mountmgr-identify-optical-directory-maps.patch` reconoce ISO9660/UDF
al crear dispositivos para directorios mapeados, incluso sin un dispositivo Unix
`d::`. Conserva las preferencias explícitas de `Software\Wine\Drives`. Hay que
instalar juntos `mountmgr.so` y `mountmgr.sys.so`: comparten una estructura interna.
`tools/update_user_image.py` incluye ambos módulos. La prueba Win32
`tests/optical-drive.c` comprueba C:, D:, el formato antiguo `d:`, el dispositivo
CdRom de mountmgr y la lectura del ejecutable del CD original de Quake II.
La evidencia en `evidence/quake2-newgame-optical-2026-10-08` incluye una partida
nueva renderizada con VirGL; la imagen VMware está actualizada pero su partida
requiere confirmación en VMware. El fallo anterior de CD era intermitente, por
lo que esta prueba no demuestra que todos los congelamientos reportados estén
resueltos.
