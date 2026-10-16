# Arquitectura y fronteras ABI

Objetivo: EXE de Windows -> Wine upstream PE/ntdll -> wineserver y runtime Unix
i386 -> int 0x80 Linux -> kernel BlesK. El procesador i386 ejecuta instrucciones
nativamente. QEMU se usa aqui para probar el sistema completo; no se agrego un
emulador de CPU a BlesK.

Ruta heredada independiente: EXE -> `kernel/pe_loader.c` ->
`kernel/win32/win32.c` y sus implementaciones/resolvers. `programs/wine.c` sigue
en esta ruta. Las etapas `wine_stage5..9` no son una distribucion upstream.

Cambios de esta serie:

1. `kernel/linux/i386_translate.inc` separa la tabla de traduccion de la
   implementacion nativa. `syscall_handler` captura los argumentos originales,
   conserva EBX/ECX/EDX/ESI/EDI/EBP y distingue el frame del llamante del devuelto
   por el scheduler. Las llamadas desconocidas retornan -ENOSYS.
2. `kernel/linux/i386_fs.inc` reutiliza recursos y FD del dispatcher para
   `openat`, `fstatat64` y `writev`. No crea otra tabla de descriptores.
3. El cargador entrega a un PT_INTERP ordinario imagenes sin preenlazar:
   glibc resuelve sus dependencias, TLS, relocaciones y constructores. El
   interprete didactico conserva su ruta mediante nota ELF BLESK tipo 0x6b01,
   descriptor version 1. Al reconstruir imagenes se debe actualizar ld-blesk.so.
4. La pila inicial aporta AT_RANDOM, AT_EXECFN, AT_FLAGS, AT_SECURE y AT_CLKTCK,
   ademas del auxv previo; conserva alineacion de 16 bytes. El lanzamiento nativo
   inicial sigue llevando un argv y entorno vacio; execve ahora prepara argv/envp
   validados y conserva PID mediante commit de una imagen suspendida.
5. mmap admite reemplazo MAP_FIXED contenido en una reserva privada existente,
   conservando paginas vecinas. No implementa MAP_FIXED general. MAP_SHARED usa objetos VFS referenciados y frames comunes a los aliases y al fork; msync integra writeback FAT.
   Los limites confiables de las imagenes ELF quedan en metadata supervisor;
   mprotect usa esos limites para RELRO de main/ld.so, no headers modificables.
6. El array de consultas legacy de tareas pasa de BSS a memoria supervisor
   asignada al iniciar el scheduler. Resuelve un fallo de enlace preexistente
   comprobado tambien con syscall.c original; no se cambio el limite del linker.
7. `i386_signal.inc` implementa contexto i386, mascaras y acciones compartidas;
   `i386_futex.inc` espera atomica y robust lists. El retorno comun entrega las
   senales despues de seleccionar CR3/FPU. La entrada int 0x80 deshabilita IRQ
   hasta guardar segmentos y luego restaura IF; evita perder GS/TLS por una IRQ
   entre el cambio de privilegio y la captura del frame.
8. `i386_ipc.inc` mantiene objetos pipe/socket referenciados por la misma tabla
   FD que VFS. SCM_RIGHTS conserva la descripcion abierta; un recolector elimina
   ciclos de derechos. Las esperas sueltan los dominios de kernel antes de ceder.
9. VFS conserva su ABI publico de directorios. La identidad Linux se obtiene de
   volumen/registro FAT y una tabla supervisor que crece bajo demanda. Renombrar
   mueve el registro conservando el inode; fdopendir/readdir usan cookies de la
   descripcion compartida. No se modifico glibc para resolver dlopen.
10. El build usa secciones por funcion/dato, --gc-sections y -Oz. Desktop y widget
    tambien usan -Oz; los bucles graficos principales siguen con -O2. Esto permite
    mantener el crecimiento acotado. La reserva pasa a 1280 KiB; BSS sigue al archivo y conserva sus asserts. Ver KERNEL_LAYOUT.md.

El interprete real no debe compartir `/lib/libc.so.6` con la biblioteca de
pruebas BlesK. El paquete actual usa `/WINEGATE/lib` y RPATH explicitamente.
Esta ruta es de pruebas; el destino de Wine sera `/opt/wine` con un prefix
persistente separado. No se ha creado un prefix falso.

Graficos previstos: winex11.drv upstream -> libX11 compatible con Linux ->
puente explicito con handles enteros y copias validadas -> GUI/compositor
existentes. No se debe enlazar `sdk/lib/xlib.c` sin adaptar su ABI. Un servidor
X11 local por protocolo sigue siendo alternativa si el shim resulta insuficiente.

Referencias primarias revisadas: [Wine 11.0](https://github.com/wine-mirror/wine/tree/wine-11.0),
su [loader](https://github.com/wine-mirror/wine/blob/wine-11.0/loader/main.c),
[IPC de ntdll](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/server.c)
y [contextos i386](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/signal_i386.c).
La implementacion propia consulta tambien el ABI publico de Linux:
[frames de senales i386](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/signal_32.c),
[sigcontext](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/uapi/asm/sigcontext.h)
y [flags de filesystem](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/fcntl.h).
Referencias secundarias: [configuracion BoxedWine](https://github.com/danoon2/Boxedwine/blob/master/tools/buildWine/wine_builds.json)
y [checklist de Serenity](https://github.com/SerenityOS/serenity/issues/6410).
No se copio implementacion GPL de BoxedWine al kernel.

