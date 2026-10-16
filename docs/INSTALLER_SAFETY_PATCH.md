# BlesKernOS 0.9 — parche instalador de CD: seguridad, MBR, streaming y espacio

**Base:** `BlesKernOS 0.9.rar` del proyecto (11 de octubre de 2026). Aplicar únicamente sobre esta revisión o resolver conflictos con cambios posteriores.

## Cambios realizados

- La selección de destinos rechaza discos con GPT (MBR protector 0xEE o cabecera «EFI PART» en LBA 1), MBR inválidos, solapamientos, límites incorrectos y flags de arranque desconocidos. Un disco que no pueda leerse no se considera vacío. Se endurece también el acceso a particiones del instalador multidisquete.
- Modos separados: **instalación limpia**, **actualización** (sin limpieza recursiva) y **reparación** (reponer archivos de bootstrap que faltan o difieren del CD, comparando contenido). No se elimina `/DOCUMENTS` en modos no destructivos.
- Para partición primaria MBR, se verifica chainloader BIOS y partición activa. Si hay que modificar MBR/activo, se pide escribir `ARRANQUE`; se almacena el sector original en `/SYSTEM/SETUP/MBR.BAK`, se preservan ID de disco y tabla de particiones y se comprueba el sector escrito. La versión de `boot/mbr.asm` incluida en la ISO es la usada para la validación.
- Copia ISO → VFS en bloques de 16 KiB (archivos temporales `.BKNEW` y posterior `rename`). Sector reservado/Stage2/kernel con bloques de 512 bytes, sin cargar kernel completo en una reserva `kmalloc`.
- BKL3: descompresión incremental con ventana de 4 KiB y buffer de salida de 4 KiB, CRC32 incremental, fichero temporal y commit tras verificación. El VFS/BFS4 conserva sus propias cachés internas.
- Preflight dinámico de espacio desde `SYSTEM/SETUP/MANIFEST.TXT`: tamaño bootstrap + suma de archivos expandidos + 16 MiB; para formateo, reserva adicional del 12,5 % para metadatos. Comprueba archivos de arranque y cabeceras BKL3 **antes de formatear**.
- El disco de prueba QEMU se crea de manera dispersa con `truncate` y el tamaño por defecto sube de 64 MiB a 1536 MiB.

## Aplicación

Desde el directorio raíz del source (el que contiene `Makefile`):

```bash
patch -p1 --dry-run < BlesKernOS-0.9-Installer-Safety-Streaming.patch
patch -p1 < BlesKernOS-0.9-Installer-Safety-Streaming.patch
python3 tests/installer/test_installer_safety.py
make iso-user
```

Para probar SIN hardware real:

```bash
make reset-installer-target INSTALL_TARGET_MB=1536
make run-iso INSTALL_TARGET_MB=1536
# Completar Setup en QEMU; luego sin CD:
make run-installed INSTALL_TARGET_MB=1536
```

**Importante:** `make run-iso` no cambia el tamaño de una imagen `installer-target.img` ya existente. Regenerarla con `reset-installer-target` solo si contiene datos descartables.

## Límites y pruebas pendientes

- El código modificado compiló a objetos de 32 bits con GCC (`-Wall -Wextra -Werror`) y superó pruebas de detección de discos simulados en RAM. **No se ejecutó un arranque completo ni la instalación en QEMU o en la Acer TravelMate 250 en este entorno.**
- El preflight comprueba cabeceras BKL3, pero no verifica cada CRC de los paquetes antes de formatear. La CRC completa se comprueba durante el despliegue. CD dañado o desconexión pueden dejar instalación incompleta.
- El uso de temporales + rename reduce el riesgo de reemplazos parciales, pero **no garantiza transacciones ni recuperación después de un corte de luz**; falta trabajar BFS4/fsck y checkpoints.
- La reparación de CD corrige **bootstrap y sectores de arranque**, no todos los archivos del sistema que SETUP.BEX ya haya expandido desde BKL3.
- Una actualización vuelve a colocar `START.INI` de Setup y puede requerir volver a pasar por el asistente. Hacer backup de configuraciones antes de actualizar.
- El MBR del CD utiliza BIOS EDD (`INT 13h AH=42h`); no prueba que el BIOS viejo tenga ese servicio.
- El espacio requerido es conservador: incluye todos los BKL, incluso Wine opcional, más la expansión. Puede rechazar volúmenes que alcanzarían si se midiesen las opciones elegidas.
- La escritura por streaming grande se probó a nivel de código para BFS4; FAT32 puede emplear un buffer VFS mayor y requiere pruebas específicas.
- Si la rama local posterior al RAR modificó `installer.c`, `bkl_setup.c`, `floppy_installer.c` o el Makefile, revisar los conflictos antes de aplicar.

## Pruebas recomendadas antes de tocar el HDD de la Acer

1. ISO El Torito en QEMU → disco virtual vacío → primer arranque sin CD → completar Setup.
2. Instalar en segunda partición primaria MBR: verificar sectores de la primera partición sin modificar, MBR.BAK guardado, único flag activo y chainloading correcto.
3. Discos GPT con MBR protector, GPT sin protector, MBR inválidos y particiones superpuestas: deben ser rechazados sin escribir.
4. Actualización/reparación con `/DOCUMENTS/important.txt`: comparar hash antes y después.
5. Disco virtual menor al espacio que indica el manifiesto: rechazo ANTES de formatear.
6. BKL3 de archivo grande y con CRC incorrecto: no dejar sustituto incompleto ni marcar Setup completo.
7. Cortar la VM en varias etapas y comprobar que no se modifica información de otras particiones ni se ocultan errores.
8. Solo entonces probar HDD PATA real + lector ATAPI y arranque BIOS de TravelMate 250.
