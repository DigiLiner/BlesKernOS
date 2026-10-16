# Gráficos X11: primer hito Wine 11.0

TinyX/KDrive i386 ejecuta sobre la GUI de BlesKernOS mediante
`/dev/blesgfx`. El port y sus licencias están en [ports/tinyx](../../tinyx/README.md).
`libX11.so.6` i386 se conecta al socket Unix real del servidor. El cliente
nativo probó `XOpenDisplay`, ventana, rectángulo y evento `Expose`, con captura
en [xlib-rectangle.png](../../tinyx/evidence/xlib-rectangle.png).

En una copia de un prefijo Wine 11.0 ya inicializado, un PE Win32 de consola
salió con código 0 y otro PE llamó `MessageBoxA`. `winex11.so` se cargó,
TinyX mapeó la ventana, el cuadro se vio en el framebuffer y Wine devolvió
`IDOK`. El gate KVM/AHCI/1536 MiB terminó con código 0; ver
[messagebox.png](../../tinyx/evidence/messagebox.png) y
[messagebox-result.json](../../tinyx/evidence/messagebox-result.json).

La prueba no equivale aún a una sesión de escritorio Wine completa. El DDX
usa 640×480 XRGB32, copia completa cada 50 ms, mapa de teclado ASCII básico
y una cola de 64 eventos. Tecla y clic PS/2 llegaron a un cliente X; falta
verificar `MotionNotify` y ampliar modos y extensiones. La prueba final
reutilizó el prefijo de `w3-profile-baseline-reboot-05`; una prueba separada
de prefijo fresco emitió `run_wineboot boot event wait timed out`, de modo
que el primer `wineboot --init` con TinyX todavía no tiene gate aprobado.

`sdk/lib/xlib.c` continúa siendo un prototipo del SDK nativo, distinto de la
ABI ELF de `libX11.so.6` usada por Wine. WineD3D/OpenGL y Office quedan fuera
del alcance de este hito.
