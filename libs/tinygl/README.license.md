# TinyGL adaptado para BlesKernOS

Los archivos de TinyGL en `libs/src/`, los encabezados en `libs/tinygl/` y
sus copias de interfaz en `sdk/include/TGL/` y `sdk/include/tinygl/` proceden
de TinyGL y contienen modificaciones para BlesKernOS. No son una distribución
upstream sin modificar. Entre las modificaciones locales están la integración
con las APIs del OS, el transporte GFX3D y el rasterizador especializado.
`libs/src/font8x8_basic.h` conserva su aviso independiente de dominio público.

Se recuperó el aviso completo de la familia C-Chads, cuyos nombres de funciones,
macros y estructura aparecen en esta copia. No se ha identificado la revisión
exacta importada. `UPSTREAM.json` fija la referencia comprobada
`36a7987e7bebfda19615ea33341b1cc0ff9c3b13` y los hashes del snapshot local
revisado: **no** presenta esa referencia como commit de importación.
`libs/src/opinfo.h` coincide con esa referencia al normalizar espacios; los
otros archivos comparados contienen diferencias. Esto no certifica toda la
procedencia ni convierte los cambios en una copia exacta de esa revisión.

Los encabezados `TGL/gl.h` declaran que sus constantes proceden de Mesa.
El upstream de referencia conserva la misma declaración sin individualizar
la versión Mesa. El texto de Brian Paul en
`libs/Mesa-3.5/include/GL/gl.h` se conserva con el paquete de avisos como
atribución adicional del Mesa incluido, sin afirmar que ese archivo concreto
sea el ancestro exacto del encabezado TinyGL. La revisión histórica de esos
encabezados sigue sin acreditarse.

- Autores indicados por upstream: Fabrice Bellard, Gek (DMHSW), C-Chads.
- Aviso original fijado: https://github.com/C-Chads/tinygl/blob/36a7987e7bebfda19615ea33341b1cc0ff9c3b13/LICENSE
- Copia íntegra: [LICENSE](LICENSE), recuperada el 2026-10-16.
- SHA-256 del aviso: `73e1887dd4de6ca926439c24b629fab376b36343211e193e1a9fe6e4e2c69c1a`.

Es una licencia de tipo zlib con reconocimiento obligatorio en producto y
documentación; no debe sustituirse por el MIT general del proyecto. La sección
SDL del aviso corresponde a los ejemplos upstream, no incorpora SDL al kernel.

`glGetString(GL_LICENSE)` devuelve ahora el aviso íntegro mediante
`license_text.h`; los empaquetadores también lo incluyen en
`/DOCS/LICENSES.TXT`. `font8x8_basic.h` no queda bajo esta licencia.
