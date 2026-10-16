# Limpieza del source — 2026-10-16

Se eliminaron 25.070 archivos en 85 rutas, que ocupaban 34,67 GiB asignados:
imágenes experimentales de Wine/navegador/drivers, discos de instalación de
prueba, directorios de investigación y staging en `build/`, builds y `dist/`
generados de programas independientes, cachés Python y respaldos de parches
(`.orig`, `.rej` y directorios explícitos de repair/audiofix).

Se conservan el código fuente, parches deliberados `.patch`, licencias,
recursos, pruebas y evidencias documentadas fuera de `build/`, dependencias
vendorizadas, los historiales `.git`, instaladores locales y fixtures externos.
No se borraron objetos DOS históricos de terceros: algunos pueden ser entradas
de compilación sin fuente equivalente. `docs/build/` es documentación.

Imágenes conservadas en `build/`:

- `bleskernos-ata-user.img`: base existente y archivos del usuario.
- `bleskernos-ata-user-commands.img`: última imagen API 40 y comandos.
- `bleskernos-usb-user.img`, `bleskernos-cd-user.iso`,
  `bleskernos-vmware-user.vmdk`: medios principales existentes.
- `bleskernos-travelmate250.img`, `bleskernos-travelmate250.iso`: variante física.

Conservar una imagen no la actualiza ni certifica que tenga los últimos cambios.
Las imágenes existentes permanecen intactas. Los objetos de compilación activos
se mantienen para permitir builds incrementales. Las reglas `.gitignore` de las
dos raíces excluyen también builds de extras, staging, fixtures generados y
respaldos de reparación para que no vuelvan a contaminar el source.

## Reconstrucción

Los Makefiles regeneran los directorios de build y las imágenes generadas de
extras. `make extra-programs-image` reconstruye `programas-extra.img`.
Los tests Wine con `--base` usan la imagen ATA principal por defecto; los tests
que requieran juegos instalados necesitan una base con esa instalación.
`tests/kernel/run_wine_memory.py` también necesita los archivos locales de
WinZip bajo `build/wine-memory/`: deben aportarse de nuevo para esa prueba.
Las referencias históricas a logs de `build/` en los informes Wine describen
ejecuciones anteriores; esos logs temporales ya no se conservan.

Para generar una copia con los comandos actuales y los avisos actualizados:

```sh
python3 tools/package_commands_image.py build/bleskernos-ata-user-commands.img build/terminal-nueva.img
```

La fuente debe estar detenida y el destino debe ser nuevo. No se genera una
nueva copia automáticamente durante esta limpieza.

## Licencias

La revisión se documenta en [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).
Se agregaron las bibliotecas del navegador, BearSSL, Mesa 3.5, PL_MPEG, TinyX y
herramientas opcionales, y se recuperó el aviso TinyGL con su procedencia y
limitaciones. Los empaquetadores agregan `/DOCS/LICENSES.TXT`; siguen enviando
por separado los archivos de licencia y fuente de los componentes que ya los
incluían. Las imágenes guardadas requieren actualización para recibirlo.

Siguen sin resolverse los permisos de Actua Soccer/HMI, la revisión exacta
de TinyGL y sus encabezados, y la procedencia de parte del arte KDE/React95.
Esta revisión no certifica todas las licencias del árbol.
