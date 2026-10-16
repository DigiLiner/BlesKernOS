# Organización del source 0.9

Los comandos de compilación se ejecutan desde la raíz del proyecto. `Makefile`
y `GNUmakefile` siguen siendo los puntos de entrada de las builds normales.

| Carpeta | Contenido |
| --- | --- |
| `boot/`, `arch/`, `kernel/` | Arranque, plataformas y [núcleo organizado por subsistema](../kernel/README.md) |
| `gui/`, `system/`, `programs/` | Escritorio, servicios, comandos y aplicaciones integradas |
| `sdk/`, `libs/`, `third_party/` | SDK, bibliotecas y dependencias externas |
| `assets/`, `config/desktop/` | Recursos y plantillas `Desktop.INI` / `Associations.INI` |
| `ports/` | Ports de Wine y TinyX, scripts y evidencias de sus pruebas |
| `extras/` | Aplicaciones independientes; conservan sus propios repositorios |
| `mk/` | Makefiles auxiliares de Raspberry Pi, self-hosting y Codex Pro |
| `tools/` | Herramientas de build, empaquetado y diagnóstico |
| `tools/maintenance/` | Scripts históricos de migración de fuentes |
| `tests/` | Pruebas por subsistema y datos de prueba en `fixtures/` |
| `media/installers/` | ISOs e imágenes de instaladores locales |
| `extras/archives/` | Paquetes RAR de aplicaciones independientes |
| `build/` | Objetos, ejecutables e imágenes generadas |

La imagen de fuentes se genera con `make source` en
`build/source/bleskernos-source.img`. El makefile de ARM se utiliza con
`make -f mk/rpi3.mk`. El de self-hosting está en `mk/selfhost.mk`; las imágenes
Developer y de fuentes también incluyen el acceso `/SYSTEM/SOURCE/SELFHOST.MK`.
Codex Pro se incluye mediante `mk/codex-pro.mk`.

Los archivos de configuración se empaquetan con los nombres que usa el sistema
invitado (`/Desktop.INI` y `/Associations.INI`); su nueva ubicación corresponde
al árbol de fuentes del host.

## Documentación

- [Build, ediciones y self-hosting](build/)
- [Kernel, memoria y SMP](kernel/)
- [Refuerzo de memoria y API nativa del 13 de octubre](kernel-hardening-2026-10-13.md)
- [Drivers gráficos y 3D](graphics/)
- [Almacenamiento y sistemas de archivos](storage/)
- [Compatibilidad y APIs Windows](compatibility/)
- [Escritorio y audio](desktop/)
- [Plataformas, red, impresión y USB](platform/)
- [API y referencias generales](reference/)
- [Organización del kernel y retirada de Win32 propio](build/kernel-win32-removal-2026-10-08.md)
- [Registro de la limpieza de octubre de 2026](build/source-cleanup-2026-10-08.md)

## Dónde guardar archivos nuevos

Mantener la raíz para los puntos de entrada, README y licencias. Agregar las
pruebas al subsistema correspondiente de `tests/`, la documentación a su tema
y los productos generados a `build/`. Los medios de instalación y los paquetes
RAR locales están excluidos del control de versiones. Las copias `.orig`,
`.rej`, `.pre-*` y `.bak*` no pertenecen al source activo.

Las dependencias externas y los runtimes empaquetados conservan su estructura
original. Las evidencias de rendimiento y compatibilidad siguen bajo `ports/`.

- [Fuentes reales, visor Font y API](desktop/fonts.md)
