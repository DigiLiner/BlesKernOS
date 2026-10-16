# TravelMate 250: entrega y evidencia, 2026-10-15

[Descripción, componentes, límites y pasos en el portátil](../../../../docs/platform/travelmate250.md).
[Archivos de entrega y SHA256](artifacts.json). No se modificó la imagen Wine
anterior; se crearon `build/bleskernos-travelmate250.img` y `.iso`.

| Prueba | Resultado | Alcance |
|---|---|---|
| DHCP y RTL8139, ASan/UBSan i386 | PASS | Pérdida/retransmisión, ACK parcial, paquetes inválidos, DMA que cruza el anillo, timeout, padding y bits de umbral TX |
| Accesos AC97, ASan/UBSan i386 | PASS | Ancho/dirección MMIO y transición PCI D3/D0, no codec físico |
| Fragmentos AC97 | PASS | Duración de fragmentos y reutilización del ring |
| Gen2, ASan/UBSan host/i386 | PASS | Arena, pitches, comandos 2D/3D, batching, entradas inválidas y liberación del mutex por callbacks |
| Red QEMU, 256 MiB, 1 CPU | PASS | DHCP, gateway/DNS, ping y resolución de example.com |
| Kernel final QEMU, 256 MiB, 2 CPU | PASS | Mismos casos de red/DNS y resolución de imports de drivers sin símbolos faltantes |
| Audio QEMU | PASS | Tono + PCM, captura de 229364 bytes con 164272 bytes no nulos |
| Classic y deskbar | PASS | 2500 píxeles exactos de Classic; ancho de barra 620 en pantalla 800 |
| Classic después de ambos protectores | PASS | 2500 píxeles exactos al regresar al escritorio en cada caso |
| SSPIPES y SSBALLS | PASS | Animación y cierre con Escape; no GPU Intel emulada |
| Bootstrap del instalador | PASS | Vision y Classic presentes antes de extraer paquetes |
| ISO final | PASS parcial | Arranca y entra en el instalador; sin discos duros adjuntos, no se probó instalación completa |
| Intel 852/855 físico / ICH4+CS4299 físico / RTL8101L físico | Pendiente | No hay acceso al portátil en esta sesión |

La gráfica no se presenta como un port OpenGL completo. Mantiene el modo del
firmware, acelera 2D a 16/32 bpp y tiene un subconjunto 3D a 32 bpp. El driver
sólo publica aceleración cuando sus pruebas reales fuera de pantalla pasan en
la GPU. No se validó esa rasterización en QEMU ni se deduce su velocidad de una
prueba VMware/VESA. Texturas y estados no soportados quedan en software.

La revisión final agregó un mutex compartido para TODAS las entradas 2D/3D que
usan el anillo o acceden a sus superficies. El mutex propio del core GFX3D no
serializaba el camino 2D del compositor. El loader exporta kmutex_lock/unlock
sólo dentro de su rama para imágenes supervisoras; no se amplía el API de apps.
El kernel/driver final se volvieron a compilar, la imagen y el ISO se actualizaron
y se repitieron host Gen2, red/DNS SMP y arranque del ISO después de ese cambio.

El fondo fallaba porque Vision solicitaba cambiar el propietario de la imagen
al PID actual mediante una API que sólo permite publicación con propietario 0.
El bitmap ya era propiedad del servicio, de modo que ahora permanece así sin
hacer la llamada rechazada. No se debilitó la validación del kernel.
`logs/classic-before-fix.png` conserva el escritorio azul pese al log de BMP
decodificado; `logs/audio-verified/.../desktop.png` muestra el resultado corregido.

Los resultados más antiguos de QEMU audio/protectores usaron el kernel anterior
al pequeño cambio de exports supervisores. Sus módulos de audio, Vision y
protectores son los mismos de la entrega final. `logs/locked-final/` y
`logs/iso-final/` corresponden al kernel definitivo. Los logs finales del host
corresponden a las fuentes de `source-hashes.json`.

Se conservan los archivos iniciales en `inputs/`, las fuentes finales en
`sources/` y un patch en `changes.patch`. Los logs de intentos fallidos de
construcción/debug permanecen en `build/travelmate250/`; esta carpeta de
evidencia contiene los resultados finales y el caso visual previo necesario.
