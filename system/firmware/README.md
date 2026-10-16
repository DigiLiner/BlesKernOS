# Firmware HP LaserJet

No se incluyen binarios propietarios. Para HP LaserJet 1020/1018 hace falta
el contenedor descargable del modelo exacto, obtenido de una fuente autorizada:

- `SIHP1020.DL` → USB `03f0:2b17`.
- `SIHP1018.DL` → USB `03f0:4117`.

Coloca el archivo aquí antes de construir la imagen, o cópialo dentro del OS
a `/SYSTEM/FIRMWARE/`. El empaquetador incluye únicamente los nombres conocidos.
Debe ser un archivo `.dl` con cabecera HP, no el ELF `.img`, un controlador
Windows ni firmware de otro modelo. No hay conversión ni descarga automática.

USBCLASS también identifica los contenedores SIHP1000.DL y SIHP1005.DL, pero
no hay perfil de páginas para esos modelos: su variante ZjStream es diferente.

La carga se intenta al consultar o usar el puerto USBPRN. Se valida el formato,
se envía en bloques y se espera `FWVER:` en el ID IEEE1284. Eso comprueba la
preparación, no la autenticidad del firmware. Usa una copia confiable.
Si una carga es parcial o no confirma preparación, reconecta la impresora;
el sistema no repite el firmware sobre un estado incierto. Reiniciar el OS
sin apagar la impresora conserva el firmware si el ID anuncia FWVER.

El soporte HP1020/1018 está implementado y probado con simulación de USB y
codificación/decodificación de páginas; falta verificarlo en una impresora real.
