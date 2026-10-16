# Limpieza del source 0.9 — 2026-10-08

Se reorganizó el árbol sin modificar el comportamiento del kernel ni de los
ports. La raíz conserva cinco archivos: Makefile, GNUmakefile, README.md,
LICENSE y THIRD_PARTY_LICENSES.md, además de .gitignore.

## Cambios

- 68 movimientos de archivos o carpetas: medios a `media/installers/`,
  configuración a `config/desktop/`, makefiles auxiliares a `mk/`,
  documentación por tema y pruebas por subsistema.
- `programas extras/` pasó a `extras/`; se conservó su repositorio `.git`.
  Los dos RAR se agruparon en `extras/archives/`.
- La imagen de fuentes pasó de `tests/` a
  `build/source/bleskernos-source.img`.
- Se retiraron 69 copias `.orig`, `.rej`, `.pre-*` y `.bak-*` a un respaldo
  externo; se eliminaron ocho carpetas de caché Python del árbol propio.
- Se ajustaron rutas en Makefile, herramientas, documentación y pruebas.
  Los paquetes de fuentes ahora incluyen `config/`, `mk/` y `docs/` y
  conservan el acceso invitado `/SYSTEM/SOURCE/SELFHOST.MK`.
- Se amplió .gitignore para medios locales y residuos de migraciones.

## Validación

Pasaron las pruebas de color/presentación ATI, envío PIO, reserva del kernel,
libc i386 y lectura/escritura de archivos grandes BFS4/VFS. La prueba de BSS
estaba desactualizada: suponía que 3 MiB desbordaban la reserva, pero el linker
actual permite 4 MiB. Ahora comprueba 3 MiB válidos y 5 MiB rechazados;
el linker y el kernel no cambiaron.

Se verificaron los planes de build (`make -n`) del kernel, User, imagen de
fuentes, Raspberry Pi y kernel self-hosting. También se comprobó el empaquetado de fuentes con
escrituras simuladas, la sintaxis Python afectada y los enlaces Markdown.
No se realizó una build completa ni una prueba de arranque.

La imagen `build/bleskernos-ata-user.img` conservó su SHA-256 antes, después
de la reorganización y al finalizar las comprobaciones:

`f9729d3c085deb5f89c8ffb0f5c229b9557e05b0c5c967693a6e3e7284466f4f`

Las imágenes existentes mantienen su contenido; la nueva estructura de
fuentes se incorporará al generar de nuevo las imágenes correspondientes.

## Respaldo e inventario

Ubicación local: `/home/lucas/.local/share/bleskernos/source-cleanup/20261008-225304`.

[Inventario con rutas, tamaños y hashes](/home/lucas/.local/share/bleskernos/source-cleanup/20261008-225304/manifest.json).
`archived/` conserva las 69 copias retiradas; `before-edits/` conserva
los archivos antes de modificar sus referencias. El inventario registra
los movimientos para poder deshacerlos. Los instaladores, discos, runtimes,
dependencias externas y evidencias de Wine se conservaron.
