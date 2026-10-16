# Memoria virtual por proceso - Fase 1

La Fase 1 deja de implementar `mmap()`/`mmap2()` y `brk()` como simples
punteros dentro del heap global identity-mapped.

## Cambios

- Se reserva un pool fisico de paginas de 4 KiB al final de la RAM contigua.
  En equipos con poca memoria no se reserva nada; desde 48 MiB se usa 1/8 de
  la region, limitado a 8..32 MiB.
- Ese pool queda fuera del heap publicado como PAGE_USER, por lo que Ring 3 no
  puede alcanzar el backing por su direccion fisica/identity.
- Cada proceso obtiene tablas privadas bajo demanda.
- `mmap()` elige una VA privada entre 0x60000000 y 0x77ffffff y puede respetar
  un hint si esta libre. Dos procesos pueden usar la misma VA con backing
  fisico distinto.
- `munmap()` restaura la PTE supervisor identity y devuelve los frames al pool.
- `mprotect()` cambia la PTE privada del proceso en vez de tocar las tablas
  globales.
- `brk()` usa un backing privado de 4 MiB en el mismo VMM.
- El buscador de VAs reutiliza huecos liberados y las tablas privadas suben de
  12 a 128 PDEs para no limitar mappings grandes a ~48 MiB.

## Compatibilidad y Fase 2

El heap historico sigue publicado como PAGE_USER por compatibilidad. Por eso
esta fase mejora de verdad `mmap/brk`, pero aun no es aislamiento total del
proceso: `SYS_ALLOC`, stacks Ring 3, imagenes ELF nativas y mirrors antiguos
del API siguen en el heap compartido.

La Fase 2 debe migrar esas cuatro clases de memoria a mappings privados y solo
despues retirar PAGE_USER del heap general. Eso evita hacer un cambio gigante
que rompa GUI, Win16/Win32 o los ports actuales de una sola vez.
