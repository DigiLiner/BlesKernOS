# Auditoría 1.0: etapa 1 y resultados

El [informe inicial](AUDIT-1.0-2026-10-13.md) recoge estado, evidencia,
comparaciones y prioridades. Esta etapa corrige VM-03/04 conservando ABI,
formatos en disco, estructura del scheduler y afinidad Linux.

## Modificaciones verificadas

1. **Pins y overflow (`kernel/mm/pager.c`, `pager_pin`).** Se termina al
   procesar la última página del rango, antes de incrementar la dirección.
   Un rango en la última página de 32 bits ya no devuelve fallo dejando un pin
   tomado. Si esa página no pertenece al pager, tampoco envuelve la iteración
   a dirección cero. El contrato sigue dejando a `user_access_ok()` comprobar
   mappings fuera del pager; un pin exitoso no autoriza acceso por sí solo.
   Se conserva rollback cuando falla una carga intermedia.
2. **Unmap sin efectos en rangos inválidos (`pager_unmap`).** Se comprueban
   PID, longitud, alineación, overflow y posibilidad de redondeo antes de locks,
   PTEs o metadata. Los rangos válidos redondean la longitud a página para que
   retirada y cleanup cubran las mismas páginas. No se cambia el contrato de
   `paging_process_unmap` ni se pretende resolver rollback ante todos los
   errores de lifetime/shootdown.
3. **Harness de block I/O (`test_block_writes.py`).** La prueba antigua no
   compilaba por dependencias VFS añadidas al código de producción. Se agregan
   mocks explícitos y checks de desconexión, rechazo de escritura a dispositivo
   offline, rechazo de reutilización con volumen ocupado y estabilidad del
   puntero al reutilizar el slot. No se modifica el driver de bloques.
4. **Runner QEMU (`run_ram_smoke.py`).** El kernel registra Wine diferido
   aunque no arranca Wine. La prueba confundía ese mensaje con un arranque.
   Ahora excluye únicamente esa línea exacta; cualquier otro marcador Wine de
   arranque sigue fallando. Se guarda `wine_boot_started` en el resultado.

## Reproducción previa y regresión

Las pruebas `pin` y `unmap` fallaron antes de editar producción. La prueba
adicional `gap` ejecutó la copia inicial del código y detectó recorrido a VA
cero; el mock aborta ante una página fuera del rango para evitar un loop infinito.
Los originales se conservan en el directorio de evidencia. Comandos desde la
raíz del proyecto:

```sh
python3 tests/kernel/test_pager_ranges.py --source tests/kernel/evidence/audit-2026-10-13/pager-before.c pin
python3 tests/kernel/test_pager_ranges.py --source tests/kernel/evidence/audit-2026-10-13/pager-before.c unmap
python3 tests/kernel/test_pager_ranges.py --source tests/kernel/evidence/audit-2026-10-13/pager-before.c gap
python3 tests/kernel/test_pager_ranges.py
python3 tests/kernel/test_swap_pager.py
make -j4 build/kernel.bin build/stage2.bin
```

Los tres comandos con `--source` original deben fallar: son reproducciones,
no regresiones verdes. La prueba final cubre pins equilibrados, carga fallida,
cruces de página, rango desalineado, overflow, redondeo y rechazo sin mutaciones.
Después del primer cambio pasaron pin y la regresión de pager/swap; después del
segundo se ejecutó la matriz host y QEMU.

**22/22 programas de prueba host terminaron con código 0.** Incluyen pager/swap,
lookup/cache, paging/OOM/TLB, heap y accounting, stacks con guards, handoff SMP,
signals y cleanup, ABI nativa i386, protección VM, escritura block/ATA/DMA,
recuperación BFS4, operaciones BFS4 concurrentes, journal con interrupciones
por operación y swap file. La ejecución inicial conserva un fallo de compilación
del harness block; el resultado final registra su reejecución corregida.
Algunas pruebas son de contratos/texto o usan mocks: el conteo no mide cobertura
integral ni demuestra SMP real en hardware.

**Build:** kernel y Stage 2 compilaron. Kernel: 825388 bytes, SHA-256
`018f28042a78177a8182e5478a6e288761bb64d34fa089dd4a87df41d4ab2de5`.
El build registró función de setup no usada y `.note.GNU-stack` ausente en
`isr_stubs.o`; quedan documentados, no se corrigieron en esta etapa.

| QEMU TCG, VGA std | Resultado final | Qué demuestra |
|---|---|---|
| 64 MiB / 1 CPU, RAM/native | PASS | Vision/escritorio, allocation/realloc/free, ciclos de contexto gráfico y profundidades 8/16/24/32 |
| 1024 MiB / 4 CPUs, RAM/native | PASS | Misma fixture de compatibilidad en configuración SMP; no estrés adversarial de cuatro CPUs |
| 32 MiB / 2 CPUs, swap en disco temporal | PASS | Datos privados íntegros, 10743 escrituras y 8105 lecturas de swap, slots recuperados, 0 errores E/S y disable/enable |

El ensayo inicial de swap con 256 MiB devolvió FAIL: el hijo terminó con
integridad PASS pero `out=0/in=0`, sin presión suficiente para el criterio de la
fixture. Se conserva ese resultado. Se ejecutó la prueba de intercambio con
32 MiB para provocar presión efectiva; no se transforma el FAIL inicial en PASS.
Las primeras ejecuciones nativas completaron sus checks y después fallaron por
el marcador Wine diferido; las reejecuciones con runner corregido terminaron 0.

## Evidencia y límites

`tests/kernel/evidence/audit-2026-10-13/` contiene originales, hashes iniciales,
logs antes/después, `stage1.patch`, `host-results.json`,
`host-final-results.json`, build y logs/JSON/capturas QEMU.
`result.json` contiene hashes finales y la matriz final. El parche incluye las
modificaciones de producción y de pruebas; los documentos se entregan aparte.
No hay commits porque este workspace carece de Git.

QEMU usa copias desechables de la imagen y disco swap temporal; las imágenes
habituales no se actualizaron. Se recompilaron los artefactos de build.
Las pruebas guest verifican regresión normal, no la explotación de punteros
adversariales ni la última VA en un proceso real. Wine no se ejecutó: solo se
comprobó que no arrancara en estas fixtures. No se certifican Wine, todos los
drivers, tolerancia a fallos físicos ni aislamiento completo.

La siguiente prioridad sigue siendo VM-01/API-01/VM-02: proteger metadata fuera
del heap de usuario, validar argumentos tipados y probar el lifetime de espacios
y backing antes de cambiar su liberación. Esos riesgos no quedan resueltos por
los dos cambios del pager y ningún subsistema se declara completo.
