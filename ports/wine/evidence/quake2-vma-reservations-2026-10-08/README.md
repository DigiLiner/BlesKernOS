Quake II: transición de demo a nueva partida en VMware SVGA3D

La versión anterior reproducía `Couldn't reopen ./baseq2/pak0.pak` tras
`demomap demo1.dm2`, esperar 200 frames y ejecutar
`loading; killserver; wait; newgame`. La copia instalada del paquete coincide
con el CD y quedaban handles libres. La excepción real ocurre dentro de
`calloc` de libc: el top del heap apunta a `0x63080018`, dentro del intervalo
que acababa de liberar Wine (`munmap(0x63050000, 0x800000)`).

Las reservas Wine PROT_NONE pueden existir solo en los metadatos de VM. El
asignador automático anterior consultaba únicamente las PTE y permitía
alojar memoria dentro de esas reservas. El nuevo filtro consulta también
las VMAs y brk y salta intervalos completos, sin materializar todas las
reservas en tablas de páginas. Se aplica a brk, mmap privado y cachés de
archivos/mapeos compartidos.

Tras el cambio, la misma secuencia continúa hasta más de 1500 frames GPU,
con cero fallbacks del renderizador. `vmware-newgame.png` contiene el frame
479, capturado del framebuffer resuelto por la GPU: primera partida en
base1. La prueba usa `gl_monolightmap A`, igual que el perfil VMware preparado.
El menú interactivo del usuario y Virtio todavía requieren confirmación.

Pruebas de regresión:

    python3 tests/desktop/test_vm_auto_reservations.py
    python3 tests/desktop/test_vm_protect.py

`vmware-serial.log` conserva varias ejecuciones; la última corresponde al
kernel corregido. Los registros `before-*` corresponden a la reproducción
anterior al arreglo. No se agregaron logs de diagnóstico al kernel publicado.
