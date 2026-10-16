# Ampliacion de la biblioteca C del SDK

Esta entrega mejora la libc de aplicaciones (`LIBPOSIX.A`). No sustituye
la libc interna del kernel ni declara conformidad completa con C/POSIX.
La API nativa `LIBBLESK.A` sigue disponible junto a la capa C/POSIX.

## Funciones nuevas

- Enteros: `strtoll`, `atol`, `atoll`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`.
- Tipos de ancho maximo: `imaxabs`, `imaxdiv`, `strtoimax`, `strtoumax`.
- Estado aleatorio explicito: `rand_r` (no criptografico).
- Cadenas: `strtok_r`, `stpcpy`, `stpncpy`, `memrchr`, `strcasestr`, `strcoll`,
  `strxfrm`, `strerror_r`.

`strtol`, `strtoul` y `strtoull` comparten ahora un parser que comprueba
desbordamientos antes de multiplicar, consume el numero completo aunque
desborde, admite signos y los seis espacios ASCII, respeta bases 2..36 y
base automatica 0, y conserva `endptr` en el inicio cuando no hay conversion.
Los errores de rango devuelven el limite y fijan `errno=ERANGE`; una base
invalida fija `EINVAL`. Un prefijo `0x` sin un digito hexadecimal posterior
no se consume como prefijo. Los negativos sin signo usan aritmetica modular.

`inttypes.h` incorpora las funciones de intmax y macros de formato comunes.
`stdint.h` amplia tipos, limites y macros de constantes. `limits.h` incorpora
los limites ILP32 del SDK. `string.h` admite inclusion desde C++.

`strcoll` y `strxfrm` implementan el locale C, no ordenacion linguistica.
`strcasestr` trabaja con ASCII. `strerror_r` usa la firma POSIX/XSI con retorno
entero; no la variante GNU que retorna un puntero. Se ampliaron mensajes de
error comunes. Las funciones de coma flotante y el resto de POSIX mantienen
su alcance previo; siguen pendientes mejoras en esos subsistemas.

## Compilar, probar e instalar

```sh
python3 tests/sdk/test_sdk_libc.py
make -f Makefile build/sdk/libblesk_posix.a
python3 tools/package_libc_update.py
```

La prueba ejecuta codigo i386 real con el ABI de 32 bits, sin usar la libc del
host. Incluye casos de limites, overflow no detectado por la implementacion
anterior, `endptr`, bases, signos, divisiones y tokenizadores intercalados.
El test nativo es `sdk/examples/libc_regression.c`; se compila tambien con el
TinyCC distribuido por el selfhosting. La ejecucion de ese BEX en el OS queda
como comprobacion adicional.

`build/libc-update/` contiene `SYSTEM/SDK` con el archivo estatico, los headers
modificados y el ejemplo nativo con su Makefile. Copiarlo sobre una instalacion
con SDK completo, conservando una copia de los archivos anteriores. No se
modifican automaticamente las imagenes arrancables existentes. Las nuevas
imagenes construidas por las herramientas habituales toman la biblioteca y
los headers actualizados del arbol. `mk/selfhost.mk selfhost-sdk` incluye ahora
tambien las bibliotecas POSIX y matematica.
