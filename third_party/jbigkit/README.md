# JBIG-KIT para PrintSpool

Autor: Markus Kuhn, 1995–2014. Licencia GPL-2.0-or-later: `COPYING`.
Se conservan completos los cuatro archivos upstream y sus avisos. Procedencia
y hashes de los bytes originales en `UPSTREAM.json`. No contiene firmware HP.

Solo PrintSpool enlaza esta biblioteca; los drivers residentes no la enlazan.
El ejecutable combinado PRINTSPL.BEX se distribuye GPL-2.0-or-later.
El adaptador local `system/services/print_zjs.inc` tiene esa misma licencia.

El empaquetador instala la licencia y un archivo de fuentes recompilables
en `/DOCS/PRINTGPL.TXT` y `/DOCS/PRINTSRC.TGZ`. Compilar sus fuentes con GCC
para i386 reproduce el módulo ET_REL con imports de la ABI pública del OS.
El registro de fuentes incluye JBIG-KIT, adaptador, spooler, headers del SDK
y la fuente bitmap de dominio público que usa para texto ASCII.

El encoder usa malloc/realloc y aborta si no puede reservar memoria, como
upstream. PrintSpool corre en Ring 3; un fallo queda contenido en el servicio.
No se incorporan los decodificadores al ejecutable final (secciones sin uso
se eliminan). Las pruebas host sí usan el decoder para verificar los píxeles.
