# FIFA 2000: arranque y nombres de CD, 2026-10-08

Se usó la instalación existente, primero en una copia privada de la imagen.
No se ejecutó nuevamente el instalador.

## Fallos reproducidos

- El ejecutable instalado de 280311 bytes reproduce exactamente la excepción
  `24B7A58A`, precedida por excepciones en `0042B941` (`INT 1`) dentro del
  arranque protegido SafeDisc. Ver `original-exe.log`.
- La prueba con versión Windows 98 tampoco funciona: termina en `0042D392`.
  No se conservó esa configuración en la imagen del usuario.
- La variante ejecutable incluida por el usuario en su ISO supera esa caída.
  Esto NO implementa soporte general para SafeDisc en Wine ni en el kernel.
- Después aparece «not installed correctly». La pila ubica este aviso en
  `0042F4D7`: no pudo abrir `thrash\softdrawz.dll`. La instalación tiene
  `THRASH/SOFTDR~1.DLL`, idéntico byte por byte al archivo de nombre largo.
  La lectura del registro fue comprobada y no era la causa de este segundo aviso.
- El CD tiene dos árboles: ISO primario con alias cortos y Joliet con nombres
  largos. La versión anterior del driver solo montaba el primero. También hay
  nombres largos de estadios y fuentes que la instalación mínima deja en el CD.

## Cambios

El driver elige el descriptor Joliet compatible de mayor nivel, lee su raíz y
convierte UCS-2BE a UTF-8. Se ampliaron los nombres a 256 bytes y se rechazan
registros truncados, unidades UTF-16 incompletas, separadores y sustitutos.
Se conserva la lectura de discos que solo tienen árbol primario.

`tools/repair_fifa2000.py` restaura nombres largos para archivos ya instalados,
conservando los alias existentes. No copia los datos opcionales del CD que no
estaban instalados. También agrega valores de configuración ausentes para
FIFA y permite usar el ejecutable alternativo de este ISO con hashes comprobados.
La actualización de BFS4 se verifica sobre una copia temporal y luego se publica.

Aplicado a `build/bleskernos-ata-user.img`:

- `fifa2000.exe` SHA-256:
  `4e7274613e1c54d60938a27240b39d7d81ae6ffcf5c4b1d9d05c7a1ec1b4dde6`.
- Original conservado como `fifa2000.exe.safedisc-backup` SHA-256:
  `bfdef2befa0a6b745bab7aede65609784d34dedef0ed223e6b2822928e9afd24`.
- `THRASH/softdrawz.dll`, 176128 bytes, agregado sin quitar `SOFTDR~1.DLL`.
- Registro anterior conservado en `/WINEGATE/prefix/system.reg.fifa2000-backup`.
- Solo se rellenan valores ausentes: idioma, unidad de CD y renderer `softdraw`.
- Kernel, stage2 y driver ISO9660 actualizados y verificados contra el build.

No se distribuyen archivos del juego en este directorio ni se incorporan al build.

## Validación

- `python3 tests/desktop/test_iso_read_cache.py`: PASS, con ASan y UBSan;
  cubre Joliet, nombres largos/acentos, registros inválidos, fallback primario
  y la caché/lecturas bulk existentes.
- `python3 ports/wine/evidence/fifa2000-startup-2026-10-08/test-real-iso.py`:
  PASS usando el driver real contra `media/installers/FIFA2000.iso`: `softdrawz.dll`,
  `adboards_d.t3d` y `verdana12bi.ffn` resuelven con sus nombres correctos.
- `make -j4 user-update`: PASS; verificadas las huellas del ejecutable,
  respaldo y driver, y los bytes de stage2/kernel publicados en el disco.

La copia reparada supera la excepción original y el aviso de instalación,
pero llega a una ventana negra del juego. La partida y la salida de vídeo NO
están validadas. El experimento con renderer GDI se descartó y no se aplicó
al disco del usuario. Las últimas pruebas usan Virtio/Virgl real en QEMU.
