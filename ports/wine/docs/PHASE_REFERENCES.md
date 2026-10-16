# ABI y referencias de esta fase

Implementacion propia en el kernel, sin codigo copiado de Linux ni cambios de
codigo a Wine, glibc o su cargador. Referencias de contrato:

- [mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html): visibilidad de MAP_SHARED, referencias tras close y permisos del descriptor.
- [msync(2)](https://man7.org/linux/man-pages/man2/msync.2.html): flags, rangos y writeback.
- [fcntl_locking(2)](https://man7.org/linux/man-pages/man2/fcntl_locking.2.html): propietario por proceso, rangos y liberacion por close/exit.
- [readlink(2)](https://man7.org/linux/man-pages/man2/readlink.2.html): EINVAL para nodos que no son enlaces y ENOENT para ausentes.

Wine: tag `wine-11.0`, archivo fuente y SHA256 en `versions/wine-11.0.json`.
Las llamadas observadas proceden de `loader/main.c`, `dlls/ntdll/unix/loader.c`
y `server/request.c` de ese archivo exacto. El paquete conserva LICENSE y AUTHORS.
El cambio PT_INTERP/RUNPATH pertenece al empaquetado, con hashes antes/despues.

No hay evidencia W2 suficiente para afirmar que acepta clientes Wine reales.
El error de permisos del directorio privado es un requisito del filesystem,
no una razon para parchear o desactivar la comprobacion de seguridad de Wine.
