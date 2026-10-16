# Terminal 1.2 y comandos nativos (API 40)

El shell acepta líneas de hasta 1023 bytes y 64 argumentos. Los comandos
externos reciben hasta 2047 bytes y 128 argumentos mediante la API de lanzamiento.
Las comillas conservan nombres con espacios, también al expandir un alias.
No es una shell POSIX completa: no se agregan pipes, redirecciones ni operadores
`&&`/`||` en esta actualización. `sh` ejecuta recetas simples y archivos batch.

## Ayuda

`help` lista comandos, `help COMANDO` describe sus parámetros y `help -a`
muestra los usos completos. Los comandos nativos aceptan `--help`; las
herramientas externas conservan su ayuda propia. Si faltan los archivos del
sistema, permanece la ayuda interna de recuperación.

La terminal elige las versiones ampliadas de `help`, `mem`, `ping`, `curl`,
`wget` e `ipconfig` cuando están instaladas. `benchmark`/`bench` conservan el
benchmark completo del kernel. Se eliminan los mensajes SELFHOSTDBG de los
adaptadores habituales.

## Archivos y texto

| Comando | Parámetros y comportamiento |
|---|---|
| `touch` | `[-c] archivo...`: actualiza acceso/modificación, conserva contenido; `-c` no crea |
| `copy`, `cp`, `move` | `[-n] [-v] origen destino`: `-n` rechaza sobrescritura; destino puede ser directorio |
| `cat`, `type` | `[-n] archivo...`: lectura por bloques; `-n` numera líneas |
| `more` | `[-n] archivo...`: pausa cada 20 líneas; Enter continúa, Q+Enter termina |
| `head`, `tail` | `[-n cantidad] archivo`: 10 líneas por defecto; tail busca desde el final |
| `wc` | `[-lwc] archivo...`: líneas, palabras y bytes; permite opciones combinadas |
| `grep` | `[-invc] texto archivo...`: búsqueda literal, ASCII sin distinguir caso, números, inverso o conteo |
| `diff` | `[-q] archivo1 archivo2`: diferencias por posición de línea, sin algoritmo de alineación LCS |
| `cmp` | `archivo1 archivo2`: comparación binaria, indica primer byte distinto |
| `ls`, `dir` | `[-a] [-l] [-1] [ruta]`: ocultos, detalles o nombres; opciones separadas |
| `find` | `texto [ruta]` o `[ruta] [-name patrón] [-type f|d] [-i] [-d profundidad]` |
| `tree` | `[ruta] [-d profundidad] [-type f|d] [-name patrón] [-i]` |
| `du` | `[ruta] [-d profundidad]`: suma tamaños lógicos, no bloques asignados |
| `df` | `[ruta]`: capacidad, uso y espacio libre del volumen correspondiente |
| `stat` | `ruta...`: tipo, tamaño, modo POSIX y atributos |
| `which` | `comando...`: distingue builtins del shell y busca BEX instalados |
| `sync` | sincroniza las escrituras pendientes |
| `rmdir` | `directorio...`: rechaza archivos y directorios no vacíos |
| `chmod` | `modo_octal archivo...` o `archivo` para consultar |
| `attrib` | `archivo...` o `+r|-r archivo...`: consulta/cambia bits de escritura POSIX |
| `true`, `false` | devuelven 0 y 1, respectivamente |

La copia usa un archivo temporal junto al destino y sólo lo renombra después
de completar lectura, escritura y cierre; un fallo de lectura conserva el
archivo de destino. No es una transacción persistente entre volúmenes ni una
implementación de copia recursiva. `move` intenta primero un rename nativo.

La API de directorios aún limita cada consulta a 128 entradas. Los recorridos
avisan y devuelven error al alcanzar ese límite, en lugar de presentar un
resultado incompleto como exitoso. La profundidad predeterminada es 12 y se
puede elegir hasta 32. Los patrones de nombres admiten `*` y `?`; un patrón
sin comodines conserva la búsqueda por subcadena. `grep` no interpreta regex.
Las operaciones por líneas admiten hasta 8190 bytes por línea y comunican el
error; `cat`/`type` sin numeración no tienen ese límite. `tail` usa los offsets
con signo de la API (archivos menores de 2 GiB).

Los modos que muestran `chmod` y `attrib` son metadatos POSIX persistentes:
no se añade aquí un modelo global de usuarios ni control de acceso para todas
las aplicaciones nativas. No se implementan atributos DOS oculto/sistema.
La entrada de Terminal es por líneas; la pausa de `more` requiere Enter.

`rm -f` ignora archivos ausentes, pero informa fallos de borrado y rechaza
directorios. `calc` trabaja con enteros con signo de 32 bits y rechaza overflow
y división por cero. `sleep` valida el rango antes de convertir a milisegundos.
`compress`/`extract` conservan BKZ1 RLE, con límite de 16 MiB y validación de
pares, límites y datos sobrantes. No son ZIP/gzip. `backup` comprueba tamaños
sin sumas que puedan desbordarse y rechaza rutas con NUL interno.

## Red y procesos

- `ping [-n intentos] [-w espera_ms] [-i intervalo_ms] destino`: estadísticas
  de pérdida y RTT mínimo/promedio/máximo.
- `ipconfig [/all] [/renew]`: estado de NIC, IPv4, MAC, tráfico y renovación DHCP.
- `curl [-I] [-L] [-o archivo] [-t espera_ms] URL`: HEAD, redirecciones y salida.
- `wget [-L] [-t espera_ms] URL archivo`, o `wget -o archivo URL`.
- `netstat [-a] [-t|-u]`: sockets TCP/UDP nativos, extremos, estado y RX pendiente.
  No incluye los sockets internos de Wine ni de otras pilas de compatibilidad.
- `ftp [-u usuario] [-p clave] HOST[:puerto] list [ruta]`, `get remoto local`
  o `put local remoto`: FTP pasivo binario; no FTPS/SFTP. Usa el host de control
  para la conexión de datos, sin confiar en una dirección PASV arbitraria.
- `nice PID [low|normal|high]`: consulta o cambia prioridad del proceso;
  el kernel protege procesos de sistema.
- `usb [-v]`, `lsusb [-v]`: dispositivos enumerados en el núcleo USB, con
  VID/PID, dirección, clase y topología; ya no listan sólo controladores PCI.
- `hostname [-s nombre]`: nombre local persistente, 1–63 caracteres de etiqueta
  DNS. No cambia DNS ni el hostname anunciado por DHCP.

HTTP/HTTPS conserva un límite explícito de 4 MiB por respuesta. Valida cuerpos
Content-Length y chunked, y rechaza cuerpos incompletos o codificaciones no
soportadas. HTTP >=400 no sobrescribe el destino. CLI pide `identity`; el
navegador conserva su negociación gzip independiente. `-L` admite hasta cinco
redirecciones. Los tiempos de espera son por operación del backend de red.

## DiskPart de BlesKernOS

```text
diskpart list disk
diskpart list partition
diskpart list volume
diskpart detail ata1
diskpart create ata1 1 2048 131072 fat32 /YES
format ata1p1 DATOS /YES
mount ata1p1
df /ata1p1
unmount ata1p1
diskpart delete ata1 1 /YES
```

El ejemplo usa un disco **secundario** con espacio libre suficiente. `create`
escribe una entrada MBR; no formatea ni mueve datos. INICIO y CANTIDAD son
**sectores**, no MiB. El ejemplo reserva 64 MiB desde el sector 2048. Los slots
van de 1 a 4 y los tipos ofrecidos son fat32 (0x0c), fat16 (0x0e) y linux (0x83).
`format` elige FAT12/16/32 según tamaño, por lo que debe elegirse un tipo MBR
coherente; no crea un filesystem Linux. `list volume` describe la raíz activa;
`df /DISPOSITIVO` consulta los secundarios.

Se rechazan el disco físico que contiene la raíz, sus vistas, dispositivos
ocupados o de sólo lectura, solapamientos, rangos inválidos, GPT, particiones
extendidas y volúmenes FAT/BFS4 directos. Sólo se inicializa una tabla nueva si el
sector está completamente vacío. Se conserva el boot code existente, se lee
el MBR escrito para verificarlo y se intenta restaurar el original si falla
esa verificación. El retorno distingue errores y tabla guardada que requiere
reinicio para refrescar las vistas. Esto no garantiza atomicidad ante pérdida
de energía durante una escritura de sector.

`mount` monta en `/DISPOSITIVO` sin reemplazar la raíz del sistema. `unmount`
sincroniza y bloquea el remontaje automático hasta un nuevo `mount`; rechaza
la raíz, descriptores abiertos, referencias de paginación y el cwd del llamador.
El formateo público también protege el respaldo físico completo de la raíz.

## Compilación y validación

Requiere kernel/API **40** y los comandos reconstruidos. Deben actualizarse
kernel, stage2, NETSTACK.DVR, SHELL.BEX, comandos y los encabezados del SDK.
No hace falta reconstruir aplicaciones antiguas: las APIs anteriores conservan
sus firmas. El inspector de sockets usa un registro separado, sin cambiar el
layout de `net_stack_ops_t` de los drivers existentes.

```sh
python3 tests/commands/test_filetools.py
python3 tests/commands/test_networktools.py
python3 tests/commands/test_ftp.py
python3 tests/commands/test_diskpart.py
python3 tests/commands/test_tls_headers.py
python3 tests/commands/run_smoke.py
```

Las pruebas host ejecutan código real con AddressSanitizer/UBSan y adaptadores
de archivos/red. La integración arranca una copia temporal del disco base en
QEMU/KVM, usa servidores HTTP/FTP locales y modifica exclusivamente un segundo
disco creado por la prueba. Comprueba creación/formateo/montaje/borrado, archivos
ocupados, transferencias binarias y conservación de datos ante errores.

Para generar otra imagen sin reemplazar la anterior:

```sh
python3 tools/package_commands_image.py build/bleskernos-ata-user-commands.img build/terminal-nueva.img
```

La imagen fuente debe estar detenida. El empaquetador recupera transacciones
pendientes únicamente en la copia, conserva el inicio configurado y verifica
los bytes instalados, el arranque y BFS4. Escribe un manifiesto SHA-256 junto
a la nueva imagen.
